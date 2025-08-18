/*
** $Id: lgc.c $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#define lgc_c
#define LUA_CORE

#include "lprefix.h"

#include <stdio.h>
#include <string.h>


#include "lua.h"

#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lmem.h"
#include "lobject.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"
#include "ltm.h"


/*
** Maximum number of elements to sweep in each single step.
** (Large enough to dissipate fixed overheads but small enough
** to allow small steps for the collector.)
*/
#define GCSWEEPMAX	100

/*
** Maximum number of finalizers to call in each single step.
*/
#define GCFINMAX	10


/*
** Cost of calling one finalizer.
*/
#define GCFINALIZECOST	50


/*
** The equivalent, in bytes, of one unit of "work" (visiting a slot,
** sweeping an object, etc.)
*/
// 这里WORK2MEM没有使用GCObject的大小，而是取了TValue的大小
// 因为标记清除算法工作的过程主要是以TValue对象为单位，做遍历，标记，清除等操作，这些操作就是用来衡量工作量，
// 而且有可能多个TValue在数据上对应着同一个GCObject，另外工作量也跟工作过程中是否有释放GCObject对象内存无关，
// 是否减少全局真实债务g->GCdebt无关，所以使用TValue为单位。
#define WORK2MEM	sizeof(TValue)


/*
** macro to adjust 'pause': 'pause' is actually used like
** 'pause / PAUSEADJ' (value chosen by tests)
*/
#define PAUSEADJ		100


/* mask with all color bits */
#define maskcolors	(bitmask(BLACKBIT) | WHITEBITS)

/* mask with all GC bits */
#define maskgcbits      (maskcolors | AGEBITS)


/* macro to erase all color bits then set only the current white bit */
#define makewhite(g,x)	\
  (x->marked = cast_byte((x->marked & ~maskcolors) | luaC_white(g)))

/* make an object gray (neither white nor black) */
// 设置为灰色
#define set2gray(x)	resetbits(x->marked, maskcolors)


/* make an object black (coming from any color) */
// 设置为黑色
#define set2black(x)  \
  (x->marked = cast_byte((x->marked & ~WHITEBITS) | bitmask(BLACKBIT)))


#define valiswhite(x)   (iscollectable(x) && iswhite(gcvalue(x)))

#define keyiswhite(n)   (keyiscollectable(n) && iswhite(gckey(n)))


/*
** Protected access to objects in values
*/
#define gcvalueN(o)     (iscollectable(o) ? gcvalue(o) : NULL)


#define markvalue(g,o) { checkliveness(g->mainthread,o); \
  if (valiswhite(o)) reallymarkobject(g,gcvalue(o)); }

#define markkey(g, n)	{ if keyiswhite(n) reallymarkobject(g,gckey(n)); }

#define markobject(g,t)	{ if (iswhite(t)) reallymarkobject(g, obj2gco(t)); }

/*
** mark an object that can be NULL (either because it is really optional,
** or it was stripped as debug info, or inside an uncompleted structure)
*/
#define markobjectN(g,t)	{ if (t) markobject(g,t); }

static void reallymarkobject (global_State *g, GCObject *o);
static lu_mem atomic (lua_State *L);
static void entersweep (lua_State *L);


/*
** {======================================================
** Generic functions
** =======================================================
*/


/*
** one after last element in a hash array
*/
#define gnodelast(h)	gnode(h, cast_sizet(sizenode(h)))


static GCObject **getgclist (GCObject *o) {
  switch (o->tt) {
    case LUA_VTABLE: return &gco2t(o)->gclist;
    case LUA_VLCL: return &gco2lcl(o)->gclist;
    case LUA_VCCL: return &gco2ccl(o)->gclist;
    case LUA_VTHREAD: return &gco2th(o)->gclist;
    case LUA_VPROTO: return &gco2p(o)->gclist;
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      lua_assert(u->nuvalue > 0);
      return &u->gclist;
    }
    default: lua_assert(0); return 0;
  }
}


/*
** Link a collectable object 'o' with a known type into the list 'p'.
** (Must be a macro to access the 'gclist' field in different types.)
*/
#define linkgclist(o,p)	linkgclist_(obj2gco(o), &(o)->gclist, &(p))

static void linkgclist_ (GCObject *o, GCObject **pnext, GCObject **list) {
  lua_assert(!isgray(o));  /* cannot be in a gray list */
  *pnext = *list;
  *list = o;
  set2gray(o);  /* now it is */
}


/*
** Link a generic collectable object 'o' into the list 'p'.
*/
#define linkobjgclist(o,p) linkgclist_(obj2gco(o), getgclist(o), &(p))



/*
** Clear keys for empty entries in tables. If entry is empty, mark its
** entry as dead. This allows the collection of the key, but keeps its
** entry in the table: its removal could break a chain and could break
** a table traversal.  Other places never manipulate dead keys, because
** its associated empty value is enough to signal that the entry is
** logically empty.
*/
static void clearkey (Node *n) {
  lua_assert(isempty(gval(n)));
  if (keyiscollectable(n))
    setdeadkey(n);  /* unused key; remove it */
}


/*
** tells whether a key or value can be cleared from a weak
** table. Non-collectable objects are never removed from weak
** tables. Strings behave as 'values', so are never removed too. for
** other objects: if really collected, cannot keep them; for objects
** being finalized, keep them in keys, but not in values
*/
static int iscleared (global_State *g, const GCObject *o) {
  if (o == NULL) return 0;  /* non-collectable value */
  else if (novariant(o->tt) == LUA_TSTRING) {
    markobject(g, o);  /* strings are 'values', so are never weak */
    return 0;
  }
  else return iswhite(o);
}


/*
** Barrier that moves collector forward, that is, marks the white object
** 'v' being pointed by the black object 'o'.  In the generational
** mode, 'v' must also become old, if 'o' is old; however, it cannot
** be changed directly to OLD, because it may still point to non-old
** objects. So, it is marked as OLD0. In the next cycle it will become
** OLD1, and in the next it will finally become OLD (regular old). By
** then, any object it points to will also be old.  If called in the
** incremental sweep phase, it clears the black object to white (sweep
** it) to avoid other barrier calls for this same object. (That cannot
** be done is generational mode, as its sweep does not distinguish
** whites from deads.)
*/
// 前向屏障
// Lua在大部分情况下都是使用前向屏障，即立刻把该白色对象进行标记，只有在处理Table和UserData时使用后退屏障
void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g)) {  /* must keep invariant? */
    // 标记阶段，会对该被黑色对象引用的白色对象立刻进行标记，修改它的颜色为灰色或黑色
    reallymarkobject(g, v);  /* restore invariant */
    if (isold(o)) {
      lua_assert(!isold(v));  /* white object could not be old */
      setage(v, G_OLD0);  /* restore generational invariant */
    }
  }
  else {  /* sweep phase */
    lua_assert(issweepphase(g));
    if (g->gckind == KGC_INC)  /* incremental mode? */
      makewhite(g, o);  /* mark 'o' as white to avoid other barriers */
  }
}


/*
** barrier that moves collector backward, that is, mark the black object
** pointing to a white object as gray again.
*/
// 后向屏障
void luaC_barrierback_ (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert((g->gckind == KGC_GEN) == (isold(o) && getage(o) != G_TOUCHED1));
  // 把标记阶段往后回退了一步，重新把引用该白色对象的黑色对象修改回灰色
  if (getage(o) == G_TOUCHED2)  /* already in gray list? */
    set2gray(o);  /* make it gray to become touched1 */
  else  /* link it in 'grayagain' and paint it gray */
    linkobjgclist(o, g->grayagain);
  if (isold(o))  /* generational mode? */
    setage(o, G_TOUCHED1);  /* touched in current cycle */
}


// 把需要长驻于内存的中GCObject对象从allgc链表中移出，
// 并移入到同样声明在global_State中的fixedgc链表中进行特殊管理，
// 这样在清除阶段就不会被遍历到了，就不会有一些多余的清除判断了
void luaC_fix (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(g->allgc == o);  /* object must be 1st in 'allgc' list! */
  set2gray(o);  /* they will be gray forever */
  setage(o, G_OLD);  /* and old forever */
  g->allgc = o->next;  /* remove object from 'allgc' list */
  o->next = g->fixedgc;  /* link it to 'fixedgc' list */
  g->fixedgc = o;
}


/*
** create a new collectable object (with given type, size, and offset)
** and link it to 'allgc' list.
*/
GCObject *luaC_newobjdt (lua_State *L, int tt, size_t sz, size_t offset) {
  global_State *g = G(L);
  char *p = cast_charp(luaM_newobject(L, novariant(tt), sz));
  GCObject *o = cast(GCObject *, p + offset);
  o->marked = luaC_white(g);
  o->tt = tt;
  o->next = g->allgc;
  g->allgc = o;
  return o;
}


GCObject *luaC_newobj (lua_State *L, int tt, size_t sz) {
  return luaC_newobjdt(L, tt, sz, 0);
}

/* }====================================================== */



/*
** {======================================================
** Mark functions
** =======================================================
*/


/*
** Mark an object.  Userdata with no user values, strings, and closed
** upvalues are visited and turned black here.  Open upvalues are
** already indirectly linked through their respective threads in the
** 'twups' list, so they don't go to the gray list; nevertheless, they
** are kept gray to avoid barriers, as their values will be revisited
** by the thread or by 'remarkupvals'.  Other objects are added to the
** gray list to be visited (and turned black) later.  Both userdata and
** upvalues can call this function recursively, but this recursion goes
** for at most two levels: An upvalue cannot refer to another upvalue
** (only closures can), and a userdata's metatable must be a table.
*/
// 染色函数
static void reallymarkobject (global_State *g, GCObject *o) {
  switch (o->tt) {
    case LUA_VSHRSTR:
    case LUA_VLNGSTR: {
      set2black(o);  /* nothing to visit */
      break;
    }
    case LUA_VUPVAL: {
      UpVal *uv = gco2upv(o);
      if (upisopen(uv))
        set2gray(uv);  /* open upvalues are kept gray */
      else
        set2black(uv);  /* closed upvalues are visited here */
      markvalue(g, uv->v.p);  /* mark its content */
      break;
    }
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      if (u->nuvalue == 0) {  /* no user values? */
        markobjectN(g, u->metatable);  /* mark its metatable */
        set2black(u);  /* nothing else to mark */
        break;
      }
      /* else... */
    }  /* FALLTHROUGH */
    case LUA_VLCL: case LUA_VCCL: case LUA_VTABLE:
    case LUA_VTHREAD: case LUA_VPROTO: {
      linkobjgclist(o, g->gray);  /* to be visited later */
      break;
    }
    default: lua_assert(0); break;
  }
}


/*
** mark metamethods for basic types
*/
static void markmt (global_State *g) {
  int i;
  for (i=0; i < LUA_NUMTAGS; i++)
    markobjectN(g, g->mt[i]);
}


/*
** mark all objects in list of being-finalized
*/
static lu_mem markbeingfnz (global_State *g) {
  GCObject *o;
  lu_mem count = 0;
  for (o = g->tobefnz; o != NULL; o = o->next) {
    count++;
    markobject(g, o);
  }
  return count;
}


/*
** For each non-marked thread, simulates a barrier between each open
** upvalue and its value. (If the thread is collected, the value will be
** assigned to the upvalue, but then it can be too late for the barrier
** to act. The "barrier" does not need to check colors: A non-marked
** thread must be young; upvalues cannot be older than their threads; so
** any visited upvalue must be young too.) Also removes the thread from
** the list, as it was already visited. Removes also threads with no
** upvalues, as they have nothing to be checked. (If the thread gets an
** upvalue later, it will be linked in the list again.)
*/
static int remarkupvals (global_State *g) {
  lua_State *thread;
  lua_State **p = &g->twups;
  int work = 0;  /* estimate of how much work was done here */
  while ((thread = *p) != NULL) {
    work++;
    if (!iswhite(thread) && thread->openupval != NULL)
      p = &thread->twups;  /* keep marked thread with upvalues in the list */
    else {  /* thread is not marked or without upvalues */
      UpVal *uv;
      lua_assert(!isold(thread) || thread->openupval == NULL);
      *p = thread->twups;  /* remove thread from the list */
      thread->twups = thread;  /* mark that it is out of list */
      for (uv = thread->openupval; uv != NULL; uv = uv->u.open.next) {
        lua_assert(getage(uv) <= getage(thread));
        work++;
        if (!iswhite(uv)) {  /* upvalue already visited? */
          lua_assert(upisopen(uv) && isgray(uv));
          markvalue(g, uv->v.p);  /* mark its value */
        }
      }
    }
  }
  return work;
}


static void cleargraylists (global_State *g) {
  g->gray = g->grayagain = NULL;
  g->weak = g->allweak = g->ephemeron = NULL;
}


/*
** mark root set and reset all gray lists, to start a new collection
*/
// 标记阶段的第一步
static void restartcollection (global_State *g) {
  cleargraylists(g);
  markobject(g, g->mainthread);
  markvalue(g, &g->l_registry);
  markmt(g);
  markbeingfnz(g);  /* mark any finalizing object left from previous cycle */
}

/* }====================================================== */


/*
** {======================================================
** Traverse functions
** =======================================================
*/


/*
** Check whether object 'o' should be kept in the 'grayagain' list for
** post-processing by 'correctgraylist'. (It could put all old objects
** in the list and leave all the work to 'correctgraylist', but it is
** more efficient to avoid adding elements that will be removed.) Only
** TOUCHED1 objects need to be in the list. TOUCHED2 doesn't need to go
** back to a gray list, but then it must become OLD. (That is what
** 'correctgraylist' does when it finds a TOUCHED2 object.)
*/
static void genlink (global_State *g, GCObject *o) {
  lua_assert(isblack(o));
  if (getage(o) == G_TOUCHED1) {  /* touched in this cycle? */
    linkobjgclist(o, g->grayagain);  /* link it back in 'grayagain' */
  }  /* everything else do not need to be linked back */
  else if (getage(o) == G_TOUCHED2)
    changeage(o, G_TOUCHED2, G_OLD);  /* advance age */
}


/*
** Traverse a table with weak values and link it to proper list. During
** propagate phase, keep it in 'grayagain' list, to be revisited in the
** atomic phase. In the atomic phase, if table has any white value,
** put it in 'weak' list, to be cleared.
*/
// 值采用弱引用，而键还是强引用，所以只需要标记键，而不需要标记值
static void traverseweakvalue (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = (h->alimit > 0);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    // 当值被清空后，对应的键无论是否被标记，都会被清楚
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else {
      lua_assert(!keyisnil(n));
      // 只需要标记键，不需要标记值
      markkey(g, n);
      if (!hasclears && iscleared(g, gcvalueN(gval(n))))  /* a white value? */
        hasclears = 1;  /* table will have to be cleared */
    }
  }
  if (g->gcstate == GCSatomic && hasclears)
    linkgclist(h, g->weak);  /* has to be cleared later */
  else
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
}


/*
** Traverse an ephemeron table and link it to proper list. Returns true
** iff any object was marked during this traversal (which implies that
** convergence has to continue). During propagation phase, keep table
** in 'grayagain' list, to be visited again in the atomic phase. In
** the atomic phase, if table has any white->white entry, it has to
** be revisited during ephemeron convergence (as that key may turn
** black). Otherwise, if it has any white key, table has to be cleared
** (in the atomic phase). In generational mode, some tables
** must be kept in some gray list for post-processing; this is done
** by 'genlink'.
*/
// 代表键采用弱引用，而值还是强引用，所以只需要标记值，而不需要标记键
static int traverseephemeron (global_State *g, Table *h, int inv) {
  int marked = 0;  /* true if an object is marked in this traversal */
  int hasclears = 0;  /* true if table has white keys */
  int hasww = 0;  /* true if table has entry "white-key -> white-value" */
  unsigned int i;
  unsigned int asize = luaH_realasize(h);
  unsigned int nsize = sizenode(h);
  /* traverse array part */
  for (i = 0; i < asize; i++) {
    if (valiswhite(&h->array[i])) {
      marked = 1;
      reallymarkobject(g, gcvalue(&h->array[i]));
    }
  }
  /* traverse hash part; if 'inv', traverse descending
     (see 'convergeephemerons') */
  for (i = 0; i < nsize; i++) {
    Node *n = inv ? gnode(h, nsize - 1 - i) : gnode(h, i);
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else if (iscleared(g, gckeyN(n))) {  /* key is not marked (yet)? */
      hasclears = 1;  /* table must be cleared */
      if (valiswhite(gval(n)))  /* value not marked yet? */
        hasww = 1;  /* white-white entry */
    }
    else if (valiswhite(gval(n))) {  /* value not marked yet? */
      marked = 1;
      reallymarkobject(g, gcvalue(gval(n)));  /* mark it now */
    }
  }
  /* link table into proper list */
  if (g->gcstate == GCSpropagate)
    linkgclist(h, g->grayagain);  /* must retraverse it in atomic phase */
  else if (hasww)  /* table has white->white entries? */
    linkgclist(h, g->ephemeron);  /* have to propagate again */
  else if (hasclears)  /* table has white keys? */
    linkgclist(h, g->allweak);  /* may have to clean white keys */
  else
    genlink(g, obj2gco(h));  /* check whether collector still needs to see it */
  return marked;
}


// 遍历table，标记所有可达的结点
static void traversestrongtable (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  unsigned int i;
  unsigned int asize = luaH_realasize(h);
  for (i = 0; i < asize; i++)  /* traverse array part */
    markvalue(g, &h->array[i]);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else {
      lua_assert(!keyisnil(n));
      markkey(g, n);
      markvalue(g, gval(n));
    }
  }
  genlink(g, obj2gco(h));
}


// 遍历table，标记所有可达的结点
static lu_mem traversetable (global_State *g, Table *h) {
  const char *weakkey, *weakvalue;
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  TString *smode;
  markobjectN(g, h->metatable);
  // 1)"k": 声明键为弱引用，当键被置空时，table会清除这个结点
  // 2)"v": 声明值为弱引用，当值被置空时，table会清除这个结点
  // 3)"kv": 声明键和值都为弱引用，当其中一个被置空时，table会清除这个结点
  if (mode && ttisshrstring(mode) &&  /* is there a weak mode? */
      (cast_void(smode = tsvalue(mode)),
       cast_void(weakkey = strchr(getshrstr(smode), 'k')),
       cast_void(weakvalue = strchr(getshrstr(smode), 'v')),
       (weakkey || weakvalue))) {  /* is really weak? */
    if (!weakkey)  /* strong keys? */
      traverseweakvalue(g, h);
    else if (!weakvalue)  /* strong values? */
      traverseephemeron(g, h, 0);
    else  /* all weak */
      // 代表键与值都为弱引用，直接把当前这个table链接到待gc垃圾回收的链表中，后面若没有其它对象标记这个表内的元素，则将被全部清除
      linkgclist(h, g->allweak);  /* nothing to traverse now */
  }
  else  /* not weak */
    // 说明没设置__mode，都是强引用
    traversestrongtable(g, h);
  return 1 + h->alimit + 2 * allocsizenode(h);
}


// 遍历full user data，标记所有可达的结点
static int traverseudata (global_State *g, Udata *u) {
  int i;
  markobjectN(g, u->metatable);  /* mark its metatable */
  for (i = 0; i < u->nuvalue; i++)
    markvalue(g, &u->uv[i].uv);
  genlink(g, obj2gco(u));
  return 1 + u->nuvalue;
}


/*
** Traverse a prototype. (While a prototype is being build, its
** arrays can be larger than needed; the extra slots are filled with
** NULL, so the use of 'markobjectN')
*/
// 遍历函数类型，标记所有可达的结点
static int traverseproto (global_State *g, Proto *f) {
  int i;
  markobjectN(g, f->source);
  for (i = 0; i < f->sizek; i++)  /* mark literals */
    markvalue(g, &f->k[i]);
  for (i = 0; i < f->sizeupvalues; i++)  /* mark upvalue names */
    markobjectN(g, f->upvalues[i].name);
  for (i = 0; i < f->sizep; i++)  /* mark nested protos */
    markobjectN(g, f->p[i]);
  for (i = 0; i < f->sizelocvars; i++)  /* mark local-variable names */
    markobjectN(g, f->locvars[i].varname);
  return 1 + f->sizek + f->sizeupvalues + f->sizep + f->sizelocvars;
}


// 遍历C闭包，标记所有可达的结点
static int traverseCclosure (global_State *g, CClosure *cl) {
  int i;
  for (i = 0; i < cl->nupvalues; i++)  /* mark its upvalues */
    markvalue(g, &cl->upvalue[i]);
  return 1 + cl->nupvalues;
}

/*
** Traverse a Lua closure, marking its prototype and its upvalues.
** (Both can be NULL while closure is being created.)
*/
// 遍历lua闭包，标记所有可达的结点
static int traverseLclosure (global_State *g, LClosure *cl) {
  int i;
  markobjectN(g, cl->p);  /* mark its prototype */
  for (i = 0; i < cl->nupvalues; i++) {  /* visit its upvalues */
    UpVal *uv = cl->upvals[i];
    markobjectN(g, uv);  /* mark upvalue */
  }
  return 1 + cl->nupvalues;
}


/*
** Traverse a thread, marking the elements in the stack up to its top
** and cleaning the rest of the stack in the final traversal. That
** ensures that the entire stack have valid (non-dead) objects.
** Threads have no barriers. In gen. mode, old threads must be visited
** at every cycle, because they might point to young objects.  In inc.
** mode, the thread can still be modified before the end of the cycle,
** and therefore it must be visited again in the atomic phase. To ensure
** these visits, threads must return to a gray list if they are not new
** (which can only happen in generational mode) or if the traverse is in
** the propagate phase (which can only happen in incremental mode).
*/
// 遍历线程（协程），标记所有可达的结点
static int traversethread (global_State *g, lua_State *th) {
  UpVal *uv;
  StkId o = th->stack.p;
  if (isold(th) || g->gcstate == GCSpropagate)
    linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
  if (o == NULL)
    return 1;  /* stack not completely built yet */
  lua_assert(g->gcstate == GCSatomic ||
             th->openupval == NULL || isintwups(th));
  for (; o < th->top.p; o++)  /* mark live elements in the stack */
    markvalue(g, s2v(o));
  for (uv = th->openupval; uv != NULL; uv = uv->u.open.next)
    markobject(g, uv);  /* open upvalues cannot be collected */
  if (g->gcstate == GCSatomic) {  /* final traversal? */
    if (!g->gcemergency)
      luaD_shrinkstack(th); /* do not change stack in emergency cycle */
    for (o = th->top.p; o < th->stack_last.p + EXTRA_STACK; o++)
      setnilvalue(s2v(o));  /* clear dead stack slice */
    /* 'remarkupvals' may have removed thread from 'twups' list */
    if (!isintwups(th) && th->openupval != NULL) {
      th->twups = g->twups;  /* link it back to the list */
      g->twups = th;
    }
  }
  return 1 + stacksize(th);
}


/*
** traverse one gray object, turning it to black.
*/
// 颜色传播
static lu_mem propagatemark (global_State *g) {
  GCObject *o = g->gray;
  nw2black(o);
  g->gray = *getgclist(o);  /* remove from 'gray' list */
  switch (o->tt) {
    case LUA_VTABLE: return traversetable(g, gco2t(o));
    case LUA_VUSERDATA: return traverseudata(g, gco2u(o));
    case LUA_VLCL: return traverseLclosure(g, gco2lcl(o));
    case LUA_VCCL: return traverseCclosure(g, gco2ccl(o));
    case LUA_VPROTO: return traverseproto(g, gco2p(o));
    case LUA_VTHREAD: return traversethread(g, gco2th(o));
    default: lua_assert(0); return 0;
  }
}


static lu_mem propagateall (global_State *g) {
  lu_mem tot = 0;
  while (g->gray)
    tot += propagatemark(g);
  return tot;
}


/*
** Traverse all ephemeron tables propagating marks from keys to values.
** Repeat until it converges, that is, nothing new is marked. 'dir'
** inverts the direction of the traversals, trying to speed up
** convergence on chains in the same table.
**
*/
// 集中处理键弱引用表
// 在原子阶段本部分开始的时候，所有的Key_WeakTable都已经从gray或者grayagain中被移除并进行一些标记，
// 处理完毕后会把它们存储于g->ephemeron（译为短暂的事物）链表中，我们这里暂时称它为“E链表”。
// convergeephemerons函数的功能就是处理这个存储了Key_WeakTable的E链表，它的逻辑分以下几步：
// 1）取出：while循环分轮次进行处理，每轮外循环先从E链表取出全部的Key_WeakTable，
// 并用一个临时指针指向它表示等待处理，接着就清空g->ephemeron原链表指针；
// 2）遍历处理：内循环按某种顺序对等待处理的原E链表中的Key_WeakTable依次处理，
// 处理时若发现某个Key_WeakTable仍然具有至少一个Key和Value都为空的结点，则把该Table重新插入到下一轮的E链表中，
// 该链表在上一步被重置过已经为空链表。
// 3）反向循环：当这一轮内循环链表中所有Key_WeakTable对象全部处理结束后，若有任意对象颜色标记被修改，则外循环while重新继续开始新一轮，
// 重复第一步操作。这里还有个把dir遍历方向设置为!dir反向的优化细节
// 4）无标记改变：当一轮内循环结束后不再有任意对象标记发生改变时，convergeephemerons函数结束。
static void convergeephemerons (global_State *g) {
  int changed;
  int dir = 0;
  do {
    GCObject *w;
    GCObject *next = g->ephemeron;  /* get ephemeron list */
    g->ephemeron = NULL;  /* tables may return to this list when traversed */
    changed = 0;
    while ((w = next) != NULL) {  /* for each ephemeron table */
      Table *h = gco2t(w);
      next = h->gclist;  /* list is rebuilt during loop */
      nw2black(h);  /* out of the list (for now) */
      if (traverseephemeron(g, h, dir)) {  /* marked some value? */
        propagateall(g);  /* propagate changes */
        changed = 1;  /* will have to revisit all ephemeron tables */
      }
    }
    dir = !dir;  /* invert direction next time */
  } while (changed);  /* repeat until no more changes */
}

/* }====================================================== */


/*
** {======================================================
** Sweep Functions
** =======================================================
*/


/*
** clear entries with unmarked keys from all weaktables in list 'l'
*/
static void clearbykeys (global_State *g, GCObject *l) {
  for (; l; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *limit = gnodelast(h);
    Node *n;
    for (n = gnode(h, 0); n < limit; n++) {
      if (iscleared(g, gckeyN(n)))  /* unmarked key? */
        setempty(gval(n));  /* remove entry */
      if (isempty(gval(n)))  /* is entry empty? */
        clearkey(n);  /* clear its key */
    }
  }
}


/*
** clear entries with unmarked values from all weaktables in list 'l' up
** to element 'f'
*/
// 若Table中有未被标记的value，则把该value对应的key也设置为无效待清除状态，尽管这个key值可能在标记阶段被标记了
static void clearbyvalues (global_State *g, GCObject *l, GCObject *f) {
  for (; l != f; l = gco2t(l)->gclist) {
    Table *h = gco2t(l);
    Node *n, *limit = gnodelast(h);
    unsigned int i;
    unsigned int asize = luaH_realasize(h);
    for (i = 0; i < asize; i++) {
      TValue *o = &h->array[i];
      if (iscleared(g, gcvalueN(o)))  /* value was collected? */
        setempty(o);  /* remove entry */
    }
    for (n = gnode(h, 0); n < limit; n++) {
      if (iscleared(g, gcvalueN(gval(n))))  /* unmarked value? */
        setempty(gval(n));  /* remove entry */
      if (isempty(gval(n)))  /* is entry empty? */
        clearkey(n);  /* clear its key */
    }
  }
}


static void freeupval (lua_State *L, UpVal *uv) {
  if (upisopen(uv))
    luaF_unlinkupval(uv);
  luaM_free(L, uv);
}


// 根据对象的类型调用不同的释放函数
static void freeobj (lua_State *L, GCObject *o) {
  switch (o->tt) {
    case LUA_VPROTO:
      luaF_freeproto(L, gco2p(o));
      break;
    case LUA_VUPVAL:
      freeupval(L, gco2upv(o));
      break;
    case LUA_VLCL: {
      LClosure *cl = gco2lcl(o);
      luaM_freemem(L, cl, sizeLclosure(cl->nupvalues));
      break;
    }
    case LUA_VCCL: {
      CClosure *cl = gco2ccl(o);
      luaM_freemem(L, cl, sizeCclosure(cl->nupvalues));
      break;
    }
    case LUA_VTABLE:
      luaH_free(L, gco2t(o));
      break;
    case LUA_VTHREAD:
      luaE_freethread(L, gco2th(o));
      break;
    case LUA_VUSERDATA: {
      Udata *u = gco2u(o);
      luaM_freemem(L, o, sizeudata(u->nuvalue, u->len));
      break;
    }
    case LUA_VSHRSTR: {
      TString *ts = gco2ts(o);
      luaS_remove(L, ts);  /* remove it from hash table */
      luaM_freemem(L, ts, sizelstring(ts->shrlen));
      break;
    }
    case LUA_VLNGSTR: {
      TString *ts = gco2ts(o);
      luaM_freemem(L, ts, sizelstring(ts->u.lnglen));
      break;
    }
    default: lua_assert(0);
  }
}


/*
** sweep at most 'countin' elements from a list of GCObjects erasing dead
** objects, where a dead object is one marked with the old (non current)
** white; change all non-dead objects back to white, preparing for next
** collection cycle. Return where to continue the traversal or NULL if
** list is finished. ('*countout' gets the number of elements traversed.)
*/
// 函数会对通过参数传进来的链表按顺序进行遍历，并在遍历每一个元素的时候做如下操作：
// 1）若当前遍历的链表元素未标记，即颜色为other_white，代表该对象是垃圾对象，需要被清除，
// 于是会调用freeobj函数释放该对象的内存，并让链表指针往前进。
// freeobj函数会根据对象的类型调用不同的释放函数，
// 保证每种对象都能正确地处理并释放自身引用的其它子对象，
// 2）若当前元素已经被标记了，此时它的颜色可能是current_white当前白（原子阶段后创建的对象）
// 或者黑色（标记阶段被处理完成），则元素为合法对象，元素会继续保留，
// 并把它的颜色标记重置为current_white当前白，处理完成后，链表头部指针不变，
// 循环迭代器指针继续往前遍历指向下一个对象。、
// 
// 参数：
// 1）lua_State * L：代表当前运行该函数的线程。几乎每个函数第一个参数都是它，
// 因为很多函数运行所需要的状态都是存储于线程或者global_State中。
// 2）GCObject **p：指向GCObject * 的指针，这里可以理解为传入了一个链表，
// 链表中的元素可以通过next指针字段指向下一个元素。
// 3）int countin：代表该函数允许处理的最大对象个数。函数中会逐一遍历链表的元素，
// 每当遍历链表中一个对象的时候，计数加1，当计数达到countin时，循环结束，
// 函数的返回值为下一个要处理的对象的指针。
// 4）int countout：代表该函数执行完毕后实际处理了的对象个数。若链表对象数量足够多，
// 则countout与countin相等，否则若遍历在未达到countin数量的时候就到达了链表末端，
// 此时countout会小于countin。
static GCObject **sweeplist (lua_State *L, GCObject **p, int countin,
                             int *countout) {
  global_State *g = G(L);
  int ow = otherwhite(g);
  int i;
  int white = luaC_white(g);  /* current white */
  for (i = 0; *p != NULL && i < countin; i++) {
    GCObject *curr = *p;
    int marked = curr->marked;
    if (isdeadm(ow, marked)) {  /* is 'curr' dead? */
      *p = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr);  /* erase 'curr' */
    }
    else {  /* change mark to 'white' */
      curr->marked = cast_byte((marked & ~maskgcbits) | white);
      p = &curr->next;  /* go to next element */
    }
  }
  if (countout)
    *countout = i;  /* number of elements traversed */
  return (*p == NULL) ? NULL : p;
}


/*
** sweep a list until a live object (or end of list)
*/
// sweeptolive函数完整逻辑如下：
// 1）在每轮while循环调用中，sweeplist会被调用一次，调用时若发现当前链表第一个对象未被标记，
// 则会对它进行清除，并让g->allgc链表指针与遍历迭代器指针同时往后移动，
// 迭代初始的时候迭代器指针是指向g->allgc链表头部，然后因为这里是同时移动，所以它们仍然保持指向着同一个对象。
// 本轮sweeplist结束后sweeptolive的while循环条件（p == old）依旧满足，会继续下一轮循环的处理。
// 2）若当前链表第一个对象为已标记对象，则不执行清除，g->allgc链表指针维持不变，仍指向原来的对象，
// 不过遍历迭代器指针会继续往前进，它会指向g->allgc链表的下一个对象，所以此时sweeplist函数返回后，
// while循环条件（p == old）条件不满足，因为一个指针仍然指向g->allgc链表头部，
// 一个指针指向的是头部的下一个对象，循环结束。
// 最终，sweeptolive函数执行完毕返回后，此时的状态是：g->sweepgc指针会指向g->allgc链表中下一个需要处理的对象；
// 而g->allgc链表表头第一个对象为一个已标记的合法对象。
static GCObject **sweeptolive (lua_State *L, GCObject **p) {
  GCObject **old = p;
  do {
    p = sweeplist(L, p, 1, NULL);
  } while (p == old);
  return p;
}

/* }====================================================== */


/*
** {======================================================
** Finalization
** =======================================================
*/

/*
** If possible, shrink string table.
*/
// 如果可能，收缩字符串表
// 当一个短字符串没有被任意对象引用时，会在清除阶段被清除，同时它在strt中的指针也将被置为空指针，
// 每移除1个短字符串对象，strt的就nuse会减1，但此时strt已经分配好的内存空间其实是不会那么灵活立刻自动跟着减小的。
// checkSizes函数的作用，就是检测nuse的大小，若它已经减小到不足哈希表容量size的4分之1了，代表此时哈希表利用率较低，
// 大量空间没有被用到，则会对哈希表内存进行重新分配与调整，每次会把容量降到原来的2分之1，这样就可以节省出原来一半的内存空间。
static void checkSizes (lua_State *L, global_State *g) {
  if (!g->gcemergency) {
    if (g->strt.nuse < g->strt.size / 4) {  /* string table too big? */
      l_mem olddebt = g->GCdebt;
      luaS_resize(L, g->strt.size / 2);
      g->GCestimate += g->GCdebt - olddebt;  /* correct estimate */
    }
  }
}


/*
** Get the next udata to be finalized from the 'tobefnz' list, and
** link it back into the 'allgc' list.
*/
static GCObject *udata2finalize (global_State *g) {
  GCObject *o = g->tobefnz;  /* get first element */
  lua_assert(tofinalize(o));
  g->tobefnz = o->next;  /* remove it from 'tobefnz' list */
  o->next = g->allgc;  /* return it to 'allgc' list */
  g->allgc = o;
  resetbit(o->marked, FINALIZEDBIT);  /* object is "normal" again */
  if (issweepphase(g))
    makewhite(g, o);  /* "sweep" object */
  else if (getage(o) == G_OLD1)
    g->firstold1 = o;  /* it is the first OLD1 object in the list */
  return o;
}


static void dothecall (lua_State *L, void *ud) {
  UNUSED(ud);
  luaD_callnoyield(L, L->top.p - 2, 0);
}


// __gc元方法
// 在对象要被销毁的时候调用，一般可用于控制一些资源的释放
static void GCTM (lua_State *L) {
  global_State *g = G(L);
  const TValue *tm;
  TValue v;
  lua_assert(!g->gcemergency);
  setgcovalue(L, &v, udata2finalize(g));
  tm = luaT_gettmbyobj(L, &v, TM_GC);
  if (!notm(tm)) {  /* is there a finalizer? */
    int status;
    lu_byte oldah = L->allowhook;
    int oldgcstp  = g->gcstp;
    g->gcstp |= GCSTPGC;  /* avoid GC steps */
    L->allowhook = 0;  /* stop debug hooks during GC metamethod */
    setobj2s(L, L->top.p++, tm);  /* push finalizer... */
    setobj2s(L, L->top.p++, &v);  /* ... and its argument */
    L->ci->callstatus |= CIST_FIN;  /* will run a finalizer */
    status = luaD_pcall(L, dothecall, NULL, savestack(L, L->top.p - 2), 0);
    L->ci->callstatus &= ~CIST_FIN;  /* not running a finalizer anymore */
    L->allowhook = oldah;  /* restore hooks */
    g->gcstp = oldgcstp;  /* restore state */
    if (l_unlikely(status != LUA_OK)) {  /* error while running __gc? */
      luaE_warnerror(L, "__gc");
      L->top.p--;  /* pops error object */
    }
  }
}


/*
** Call a few finalizers
*/
// 依次执行一定个数的g->tobefnz链表中的元素的析构器，其中调用到GCTM函数，
// 即为执行__gc元方法
static int runafewfinalizers (lua_State *L, int n) {
  global_State *g = G(L);
  int i;
  for (i = 0; i < n && g->tobefnz; i++)
    GCTM(L);  /* call one finalizer */
  return i;
}


/*
** call all pending finalizers
*/
static void callallpendingfinalizers (lua_State *L) {
  global_State *g = G(L);
  while (g->tobefnz)
    GCTM(L);
}


/*
** find last 'next' field in list 'p' list (to add elements in its end)
*/
static GCObject **findlast (GCObject **p) {
  while (*p != NULL)
    p = &(*p)->next;
  return p;
}


/*
** Move all unreachable objects (or 'all' objects) that need
** finalization from list 'finobj' to list 'tobefnz' (to be finalized).
** (Note that objects after 'finobjold1' cannot be white, so they
** don't need to be traversed. In incremental mode, 'finobjold1' is NULL,
** so the whole list is traversed.)
*/
static void separatetobefnz (global_State *g, int all) {
  GCObject *curr;
  GCObject **p = &g->finobj;
  GCObject **lastnext = findlast(&g->tobefnz);
  while ((curr = *p) != g->finobjold1) {  /* traverse all finalizable objects */
    lua_assert(tofinalize(curr));
    if (!(iswhite(curr) || all))  /* not being collected? */
      p = &curr->next;  /* don't bother with it */
    else {
      if (curr == g->finobjsur)  /* removing 'finobjsur'? */
        g->finobjsur = curr->next;  /* correct it */
      *p = curr->next;  /* remove 'curr' from 'finobj' list */
      curr->next = *lastnext;  /* link at the end of 'tobefnz' list */
      *lastnext = curr;
      lastnext = &curr->next;
    }
  }
}


/*
** If pointer 'p' points to 'o', move it to the next element.
*/
static void checkpointer (GCObject **p, GCObject *o) {
  if (o == *p)
    *p = o->next;
}


/*
** Correct pointers to objects inside 'allgc' list when
** object 'o' is being removed from the list.
*/
static void correctpointers (global_State *g, GCObject *o) {
  checkpointer(&g->survival, o);
  checkpointer(&g->old1, o);
  checkpointer(&g->reallyold, o);
  checkpointer(&g->firstold1, o);
}


/*
** if object 'o' has a finalizer, remove it from 'allgc' list (must
** search the list to find it) and link it in 'finobj' list.
*/
void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt) {
  global_State *g = G(L);
  if (tofinalize(o) ||                 /* obj. is already marked... */
      gfasttm(g, mt, TM_GC) == NULL ||    /* or has no finalizer... */
      (g->gcstp & GCSTPCLS))                   /* or closing state? */
    return;  /* nothing to be done */
  else {  /* move 'o' to 'finobj' list */
    GCObject **p;
    if (issweepphase(g)) {
      makewhite(g, o);  /* "sweep" object 'o' */
      if (g->sweepgc == &o->next)  /* should not remove 'sweepgc' object */
        g->sweepgc = sweeptolive(L, g->sweepgc);  /* change 'sweepgc' */
    }
    else
      correctpointers(g, o);
    /* search for pointer pointing to 'o' */
    for (p = &g->allgc; *p != o; p = &(*p)->next) { /* empty */ }
    *p = o->next;  /* remove 'o' from 'allgc' list */
    o->next = g->finobj;  /* link it in 'finobj' list */
    g->finobj = o;
    l_setbit(o->marked, FINALIZEDBIT);  /* mark it as such */
  }
}

/* }====================================================== */


/*
** {======================================================
** Generational Collector
** =======================================================
*/


/*
** Set the "time" to wait before starting a new GC cycle; cycle will
** start when memory use hits the threshold of ('estimate' * pause /
** PAUSEADJ). (Division by 'estimate' should be OK: it cannot be zero,
** because Lua cannot even start with less than PAUSEADJ bytes).
*/
// 预充值金额 = 2 * 固定消费 - 真实消费
// 我们令当前的固定消费为100（新增消费检测值默认与它相等，所以也为100），GC完毕后真实消费剩余为160。
// 所以此时需要预充值的金额 = 2 * 100 - 160 = 40。
static void setpause (global_State *g) {
  l_mem threshold, debt;
  int pause = getgcparam(g->gcpause);
  l_mem estimate = g->GCestimate / PAUSEADJ;  /* adjust 'estimate' */
  lua_assert(estimate > 0);
  threshold = (pause < MAX_LMEM / estimate)  /* overflow? */
            ? estimate * pause  /* no overflow */
            : MAX_LMEM;  /* overflow; truncate to maximum */
  debt = gettotalbytes(g) - threshold;
  if (debt > 0) debt = 0;
  luaE_setdebt(g, debt);
}


/*
** Sweep a list of objects to enter generational mode.  Deletes dead
** objects and turns the non dead to old. All non-dead threads---which
** are now old---must be in a gray list. Everything else is not in a
** gray list. Open upvalues are also kept gray.
*/
// 遍历参数中的链表，清除其中未标记的对象，已标记的对象则把它们的年龄标记为G_OLD老对象
static void sweep2old (lua_State *L, GCObject **p) {
  GCObject *curr;
  global_State *g = G(L);
  while ((curr = *p) != NULL) {
    if (iswhite(curr)) {  /* is 'curr' dead? */
      lua_assert(isdead(g, curr));
      *p = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr);  /* erase 'curr' */
    }
    else {  /* all surviving objects become old */
      setage(curr, G_OLD);
      if (curr->tt == LUA_VTHREAD) {  /* threads must be watched */
        lua_State *th = gco2th(curr);
        linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
      }
      else if (curr->tt == LUA_VUPVAL && upisopen(gco2upv(curr)))
        set2gray(curr);  /* open upvalues are always gray */
      else  /* everything else is black */
        nw2black(curr);
      p = &curr->next;  /* go to next element */
    }
  }
}


/*
** Sweep for generational mode. Delete dead objects. (Because the
** collection is not incremental, there are no "new white" objects
** during the sweep. So, any white object must be dead.) For
** non-dead objects, advance their ages and clear the color of
** new objects. (Old objects keep their colors.)
** The ages of G_TOUCHED1 and G_TOUCHED2 objects cannot be advanced
** here, because these old-generation objects are usually not swept
** here.  They will all be advanced in 'correctgraylist'. That function
** will also remove objects turned white here from any gray list.
*/
// 遍历链表某个区间内的对象，不会去修改对象的颜色，执行以下操作：
// 3.1）清除未标记的对象；
// 3.2）已标记的对象则进行年龄的成长；
static GCObject **sweepgen (lua_State *L, global_State *g, GCObject **p,
                            GCObject *limit, GCObject **pfirstold1) {
  // 成长关系
  // 左边目标年龄，右边原始年龄
  static const lu_byte nextage[] = {
    G_SURVIVAL,  /* from G_NEW */
    G_OLD1,      /* from G_SURVIVAL */
    G_OLD1,      /* from G_OLD0 */
    G_OLD,       /* from G_OLD1 */
    G_OLD,       /* from G_OLD (do not change) */
    G_TOUCHED1,  /* from G_TOUCHED1 (do not change) */
    G_TOUCHED2   /* from G_TOUCHED2 (do not change) */
  };
  // 1）G_NEW->G_SURVIVAL；
  // 2）G_SURVIVAL, G_OLD0->G_OLD1；
  // 3）G_OLD1->G_OLD；
  // 4）G_TOUCHED1, G_TOUCHED2：与G_OLD0与G_OLD1对，区别在于他们是在标记阶段或原子阶段遍历的时候进行年龄成长，
  // 不在sweepgen函数中成长，不会随着每轮GC的执行自动成长为老对象，常用于在屏障中设置为对象的年龄；
  // 5）G_OLD：最老一代了，年龄不需要再成长；
  // 另外，通过该函数最后一个参数 * *pfirstold1我们也可以知道，sweepgen函数除了完成清除与年龄成长以外，
  // 还负责找到第一个G_OLD1年龄的元素，赋值给它
  int white = luaC_white(g);
  GCObject *curr;
  while ((curr = *p) != limit) {
    if (iswhite(curr)) {  /* is 'curr' dead? */
      lua_assert(!isold(curr) && isdead(g, curr));
      *p = curr->next;  /* remove 'curr' from list */
      freeobj(L, curr);  /* erase 'curr' */
    }
    else {  /* correct mark and age */
      if (getage(curr) == G_NEW) {  /* new objects go back to white */
        int marked = curr->marked & ~maskgcbits;  /* erase GC bits */
        curr->marked = cast_byte(marked | G_SURVIVAL | white);
      }
      else {  /* all other objects will be old, and so keep their color */
        setage(curr, nextage[getage(curr)]);
        if (getage(curr) == G_OLD1 && *pfirstold1 == NULL)
          *pfirstold1 = curr;  /* first OLD1 object in the list */
      }
      p = &curr->next;  /* go to next element */
    }
  }
  return p;
}


/*
** Traverse a list making all its elements white and clearing their
** age. In incremental mode, all objects are 'new' all the time,
** except for fixed strings (which are always old).
*/
static void whitelist (global_State *g, GCObject *p) {
  int white = luaC_white(g);
  for (; p != NULL; p = p->next)
    p->marked = cast_byte((p->marked & ~maskgcbits) | white);
}


/*
** Correct a list of gray objects. Return pointer to where rest of the
** list should be linked.
** Because this correction is done after sweeping, young objects might
** be turned white and still be in the list. They are only removed.
** 'TOUCHED1' objects are advanced to 'TOUCHED2' and remain on the list;
** Non-white threads also remain on the list; 'TOUCHED2' objects become
** regular old; they and anything else are removed from the list.
*/
static GCObject **correctgraylist (GCObject **p) {
  GCObject *curr;
  while ((curr = *p) != NULL) {
    GCObject **next = getgclist(curr);
    if (iswhite(curr))
      goto remove;  /* remove all white objects */
    else if (getage(curr) == G_TOUCHED1) {  /* touched in this cycle? */
      lua_assert(isgray(curr));
      nw2black(curr);  /* make it black, for next barrier */
      changeage(curr, G_TOUCHED1, G_TOUCHED2);
      goto remain;  /* keep it in the list and go to next element */
    }
    else if (curr->tt == LUA_VTHREAD) {
      lua_assert(isgray(curr));
      goto remain;  /* keep non-white threads on the list */
    }
    else {  /* everything else is removed */
      lua_assert(isold(curr));  /* young objects should be white here */
      if (getage(curr) == G_TOUCHED2)  /* advance from TOUCHED2... */
        changeage(curr, G_TOUCHED2, G_OLD);  /* ... to OLD */
      nw2black(curr);  /* make object black (to be removed) */
      goto remove;
    }
    remove: *p = *next; continue;
    remain: p = next; continue;
  }
  return p;
}


/*
** Correct all gray lists, coalescing them into 'grayagain'.
*/
// 该函数用来清空和重置一些分代式算法中用到的数据结构
static void correctgraylists (global_State *g) {
  GCObject **list = correctgraylist(&g->grayagain);
  *list = g->weak; g->weak = NULL;
  list = correctgraylist(list);
  *list = g->allweak; g->allweak = NULL;
  list = correctgraylist(list);
  *list = g->ephemeron; g->ephemeron = NULL;
  correctgraylist(list);
}


/*
** Mark black 'OLD1' objects when starting a new young collection.
** Gray objects are already in some gray list, and so will be visited
** in the atomic step.
*/
// 从from到to，如果节点为G_OLD1并且为黑色，则进行mark，不是黑色表明，该节点已经被mark，所以不用在mark
static void markold (global_State *g, GCObject *from, GCObject *to) {
  GCObject *p;
  for (p = from; p != to; p = p->next) {
    if (getage(p) == G_OLD1) {
      lua_assert(!iswhite(p));
      changeage(p, G_OLD1, G_OLD);  /* now they are old */
      if (isblack(p))
        reallymarkobject(g, p);
    }
  }
}


/*
** Finish a young-generation collection.
*/
// 在每次分代式算法部分执行模式执行结束后调用
// 1）调用cleargraylist函数清空重置一些GC状态数据；
// 2）调用checkSizes函数是判断短字符串缓存stringtable是否需要释放；
// 3）执行当轮被清除的对象的析构器；
static void finishgencycle (lua_State *L, global_State *g) {
  // 该函数用来清空和重置一些分代式算法中用到的数据结构
  correctgraylists(g);
  // 检测当前的短字符串缓存stringtable若使用率不及4分之1，则缩小并释放一半它的容量
  checkSizes(L, g);
  // 设置当前gc阶段为GCSpropagate：这里没有重置阶段为的第一个GC阶段GCSpause，
  // 而是跳过了它直接设置为GCSpause的下一个阶段GCSpropagate，这是因为我们知道GCSpause在进入GCSpropagate阶段前需要做的任务是：
  // 对global_State根结点进行标记，然后把标记进行传播。
  // 而分代式算法的部分执行模式在标记阶段每轮只需要把G_OLD1老一代对象的标记通过引用关系传播给他们引用的年轻一代对象，
  // 这些老一代对象就是用于作为标记传播的起始点了，所以可以跳过global_State相关的根结点标记流程，不再需要使用另外的根结点了。
  g->gcstate = GCSpropagate;  /* skip restart */
  if (!g->gcemergency)
    callallpendingfinalizers(L);
}


/*
** Does a young collection. First, mark 'OLD1' objects. Then does the
** atomic step. Then, sweep all lists and advance pointers. Finally,
** finish the collection.
*/
// 分代GC部分执行
// 收集年轻一代，当分代式GC触发时内存增长量不超过基准值时（默认为100% ），该轮GC就会使用部分执行模式
// 使用分代式算法的时候应该尽量保持程序使用部分执行模式，因为只有在该模式下，才不需要处理全部的对象，
// 比起增量式标记清除算法才具有优势。
static void youngcollection (lua_State *L, global_State *g) {
  // 部分执行模式也分标记与清除两个部分，该段代码就是标记阶段，标记阶段的核心函数就是markold函数，
  // 该函数的作用是遍历一段链表区间的对象，把其中年龄为G_OLD1的对象成长为G_OLD对象，并标记该对象的子结点。
  // G_OLD1年龄的对象属于老一代对象，他们在GC算法中属于根结点可达的有效对象，但他们当前可能还会引用其它年轻一代白色对象，
  // 不过在本轮GC结束，等待这些年轻子结点在这一轮被处理标记完毕后，他们就可以成长为最老一代G_OLD年龄对象，即真正的最老一代对象，
  // 不再引用其它未标记的年轻对象，也不再需要参与任何标记与清除的工作。
  //  标记阶段不需要处理所有的对象，只需要处理G_OLD1对象并标记它们的子结点，也正因为不需要处理全部对象，
  // 所以分代式算法部分执行模式的性能较高，
  GCObject **psurvival;  /* to point to first non-dead survival object */
  GCObject *dummy;  /* dummy out parameter to 'sweepgen' */
  lua_assert(g->gcstate == GCSpropagate);
  if (g->firstold1) {  /* are there regular OLD1 objects? */
    // allgc链表：[firstold1, reallyold)区间。
    // reallyold指针后面的对象都为最老一代对象，年龄均为G_OLD，
    // 而firstold1指针，则是在上一轮GC处理完成后缓存的指向allgc链表中的第一个G_OLD1年龄对象的指针。
    markold(g, g->firstold1, g->reallyold);  /* mark them */
    g->firstold1 = NULL;  /* no more OLD1 objects (for now) */
  }
  // finobj链表：[finobj, finobjrold]。带析构器的对象元素数量有限，这里没有采用类似的新增firstold1指针形式，
  // 节约一个指针，直接遍历处理了finobj链表头到最老一代finobjrold开始位置这区间的所有对象。
  markold(g, g->finobj, g->finobjrold);
  // tobefnz链表：整个链表。本轮GC待调用析构器后并释放的对象，从finobj链表中移入的对象，与finobj链表标记处理逻辑同理。
  markold(g, g->tobefnz, NULL);
  // 该部分为分代式算法的原子标记阶段。
  // 该函数会使用深度优先算法处理添加到gray灰色链表中的对象，对它们引用的子结点也进行标记；原子阶段同时也处理屏障和弱引用关系表的问题。
  // atomic函数调用完毕后，所有通过G_OLD1老对象引用的年轻对象都递归地完成了标记。
  // 接下来就是设置g->gcstate阶段为清除阶段GCSswpallgc，准备开始GC的对象清除工作。
  atomic(L);

  /* sweep nursery and get a pointer to its last live element */
  g->gcstate = GCSswpallgc;
  // 该部分代码多处调用了sweepgen函数，该函数的作用是，遍历链表某个区间内的对象，执行以下操作：
  // 3.1）清除未标记的对象；
  // 3.2）已标记的对象则进行年龄的成长；
  // 另外需要注意的是，sweepgen函数不会去修改对象的颜色
  psurvival = sweepgen(L, g, &g->allgc, g->survival, &g->firstold1);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->old1, &g->firstold1);
  // 上面执行完毕后，allgc链表的元素被sweepgen函数处理完毕了，对象的年龄也成长了，
  // 活着的对象又成功生存了一轮GC，接下来就通过以下代码让指针重新赋值，让allgc内各区间整体往前移，让年龄与所在链表位置重新对应，
  // 因为区间对象的年龄都成长为了下一阶段的年龄更老的对象了：
  g->reallyold = g->old1;
  g->old1 = *psurvival;  /* 'survival' survivals are old now */
  g->survival = g->allgc;  /* all news are survivals */

  /* repeat for 'finobj' lists */
  // 析构器对象的清除阶段。同理，处理finobj带析构器对象链表，与allgc链表处理一致，也是清除未标记对象，成长已标记对象，并对链表内的区间往前移
  dummy = NULL;  /* no 'firstold1' optimization for 'finobj' lists */
  psurvival = sweepgen(L, g, &g->finobj, g->finobjsur, &dummy);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->finobjold1, &dummy);
  g->finobjrold = g->finobjold1;
  g->finobjold1 = *psurvival;  /* 'survival' survivals are old now */
  g->finobjsur = g->finobj;  /* all news are survivals */

  // 同理，清除并成长tobefnz链表
  sweepgen(L, g, &g->tobefnz, NULL, &dummy);
  // 在每次分代式算法部分执行模式执行结束后调用
  finishgencycle(L, g);
}


/*
** Clears all gray lists, sweeps objects, and prepare sublists to enter
** generational mode. The sweeps remove dead objects and turn all
** surviving objects to old. Threads go back to 'grayagain'; everything
** else is turned black (not in any gray list).
*/
// 分代式算法中切换进入该模式以及全部执行模式下都会用到
static void atomic2gen (lua_State *L, global_State *g) {
  // cleargraylist函数用于初始化一些状态，重置一些变量，这里把灰色链表，弱引用表相关指针重置。
  // 接下来把当前GC阶段设置为清除第一个阶段GCSswpallgc阶段，经历了前面的标记阶段后，可达对象都已经成功标记为黑色了，
  // 这里表示要开始进入清除阶段了。
  cleargraylists(g);
  /* sweep all elements making them old */
  g->gcstate = GCSswpallgc;
  // 首先调用了sweep2old函数，该函数的作用是遍历参数中的链表，清除其中未标记的对象，已标记的对象则把它们的年龄标记为G_OLD老对象
  sweep2old(L, &g->allgc);
  /* everything alive now is old */
  // 把reallyold，old1，survival指针都指向allgc链表头部
  g->reallyold = g->old1 = g->survival = g->allgc;
  g->firstold1 = NULL;  /* there are no OLD1 objects anywhere */

  /* repeat for 'finobj' lists */
  // 与上面大致相同，不同之处是这里处理的是所有带析构器的对象，也是对其中未标记的对象进行清除，对已标记的对象则把它的年龄标记为G_OLD老对象。
  sweep2old(L, &g->finobj);
  g->finobjrold = g->finobjold1 = g->finobjsur = g->finobj;

  sweep2old(L, &g->tobefnz);

  // 正式修改GC算法类型gckind为分代式算法。然后重点操作来了：把当前实际内存分配设置为内存基准值g->GCestimate。
  // GC触发时，会使用该值与当前内存使用量进行比较，并决定是使用部分执行模式还是全部执行模式。
  g->gckind = KGC_GEN;
  g->lastatomic = 0;
  g->GCestimate = gettotalbytes(g);  /* base for memory control */
  // 在每次分代式算法部分执行模式执行结束后调用
  finishgencycle(L, g);
}


/*
** Set debt for the next minor collection, which will happen when
** memory grows 'genminormul'%.
*/
// 设置下一次年强代GC的时机
static void setminordebt (global_State *g) {
  luaE_setdebt(g, -(cast(l_mem, (gettotalbytes(g) / 100)) * g->genminormul));
}


/*
** Enter generational mode. Must go until the end of an atomic cycle
** to ensure that all objects are correctly marked and weak tables
** are cleared. Then, turn all objects into old and finishes the
** collection.
*/
// 一次性执行一遍增量式标记清除算法，执行完成后设置好对象的年龄与内存基准值
static lu_mem entergen (lua_State *L, global_State *g) {
  lu_mem numobjs;
  // 在调用切换分代式函数的此刻，当前GC算法类型必然还为另一种算法类型即增量式算法。
  // luaC_runtilstate函数用于驱使GC算法按阶段顺序、不打断地执行GC流程到某个阶段为止，此处参数为阶段枚举值GCSpause，
  // 我们知道它是GC流程的最后一个阶段，也可以理解为下一轮开始的第一个阶段。所以这里表示继续运行GC算法一直到当前轮的最后阶段GCSpause阶段才结束。
  // 
  // 分代式算法会在刚进入、以及全部执行模式结束后，会对当前内存使用量进行评估并确定一个内存基准值，
  // 后续触发分代式算法时则会根据这个基准决定该轮GC执行使用部分执行模式还是全部执行模式。
  // 所以为了让后续的算法执行模式选择更精确，于是选择了完整执行完本轮增量式算法剩余的所有流程，
  // 以让内存中的垃圾对象全部清理干净，让评估的基准值尽可能正确。
  luaC_runtilstate(L, bitmask(GCSpause));  /* prepare to start a new cycle */
  // 此时算法类型仍然还处于增量式算法类型，重新开始新一轮的GC阶段流程，并执行下列逻辑：
  // 标记阶段标记初始根结点，进入原子阶段，传播所有结点的颜色标记，并处理弱引用关系表与屏障。
  luaC_runtilstate(L, bitmask(GCSpropagate));  /* start new cycle */
  numobjs = atomic(L);  /* propagates all and then do the atomic stuff */
  // 上面执行完毕后，所有通过引用关系从根结点可达的对象都将顺利被标记为黑色，而不可达的对象则保持白色。
  // 执行前面两步的时候算法仍然都处于增量式算法模式下，都是在复用增量式算法的代码与逻辑流程。
  // 然后到了这一步，就真正准备要把算法类型切换到分代式算法
  atomic2gen(L, g);
  setminordebt(g);  /* set debt assuming next cycle will be minor */
  return numobjs;
}


/*
** Enter incremental mode. Turn all objects white, make all
** intermediate lists point to NULL (to avoid invalid pointers),
** and go to the pause state.
*/
// 清空重置一些分代式算法使用过的数据结构，然后把GC算法类型切换为增量式标记清除算法
static void enterinc (global_State *g) {
  whitelist(g, g->allgc);
  g->reallyold = g->old1 = g->survival = NULL;
  whitelist(g, g->finobj);
  whitelist(g, g->tobefnz);
  g->finobjrold = g->finobjold1 = g->finobjsur = NULL;
  g->gcstate = GCSpause;
  g->gckind = KGC_INC;
  g->lastatomic = 0;
}


/*
** Change collector mode to 'newmode'.
*/
// GC算法切换
void luaC_changemode (lua_State *L, int newmode) {
  global_State *g = G(L);
  if (newmode != g->gckind) {
    if (newmode == KGC_GEN)  /* entering generational mode? */
      entergen(L, g);
    else
      enterinc(g);  /* entering incremental mode */
  }
  g->lastatomic = 0;
}


/*
** Does a full collection in generational mode.
*/
// 分代GC全部执行模式，简而言之，其实就是一次性执行一遍增量式标记清除算法，执行完成后设置好对象的年龄与内存基准值
// 当老一代对象没有被引用时，它依旧不为白色，而且同时也跟其它老一代对象一样，不会参与到清除阶段。
// 这就导致了若我们删除一个老一代对象，该对象若引用了白色的年轻一代对象，这些年轻一代对象能得到正确的遍历与清除，
// 不过该老一代对象自身是永远不会被清除的。
// 这就导致了随着程序的运行，在部分执行模式下，内存中残留的无效老一代对象会越来越多。于是为了避免老一代对象删除问题导致的内存无限上涨，
// 分代式算法支持了另外一种模式，就是全部执行模式。
static lu_mem fullgen (lua_State *L, global_State *g) {
  enterinc(g);
  return entergen(L, g);
}


/*
** Does a major collection after last collection was a "bad collection".
**
** When the program is building a big structure, it allocates lots of
** memory but generates very little garbage. In those scenarios,
** the generational mode just wastes time doing small collections, and
** major collections are frequently what we call a "bad collection", a
** collection that frees too few objects. To avoid the cost of switching
** between generational mode and the incremental mode needed for full
** (major) collections, the collector tries to stay in incremental mode
** after a bad collection, and to switch back to generational mode only
** after a "good" collection (one that traverses less than 9/8 objects
** of the previous one).
** The collector must choose whether to stay in incremental mode or to
** switch back to generational mode before sweeping. At this point, it
** does not know the real memory in use, so it cannot use memory to
** decide whether to return to generational mode. Instead, it uses the
** number of objects traversed (returned by 'atomic') as a proxy. The
** field 'g->lastatomic' keeps this count from the last collection.
** ('g->lastatomic != 0' also means that the last collection was bad.)
*/
// 单步全部执行模式
// 全部执行模式能立刻有效地清除所有无效垃圾对象，但需要处理全部对象，所以效率很低。
// 每当执行一次全量执行模式后，分代式算法会对成功释放出来的内存量进行一个评估以决定下一次GC触发时的行为：
// 1）若释放的内存量超过了基准增长值的一半，则表示该轮全量执行模式结果良好，下
// 一次GC触发时可以重新根据内存使用量决定是否使用回部分执行模式；
// 2）若释放的内存量不及基准增长值的一半，则表示这次全部执行模式不合格，在下一次GC触发后，
// 分代式算法就会采用单步全部执行模式，该函数比起fullgen函数，有很多代码逻辑是一样的，但会多一些判断与逻辑
static void stepgenfull (lua_State *L, global_State *g) {
  lu_mem newatomic;  /* count of traversed objects */
  lu_mem lastatomic = g->lastatomic;  /* count from last collection */
  // 该部分逻辑就是把算法类型又切换到增量式算法，然后执行原子阶段流程atomic函数对所有对象进行标记，
  // atomic的返回值newatomic为原子阶段中被标记的对象个数。注意此处仅仅是标记，还未进行清除阶段流程。
  if (g->gckind == KGC_GEN)  /* still in generational mode? */
    enterinc(g);  /* enter incremental mode */
  luaC_runtilstate(L, bitmask(GCSpropagate));  /* start new cycle */
  newatomic = atomic(L);  /* mark everybody */
  // lastatomic是分代式算法中用于代表上一轮GC原子阶段中被标记的对象个数。
  // 该处判断使用了位操作右移3位（即十进制除以8），所以该处判断的逻辑是，
  // 若这一轮GC标记的对象个数没有比上一轮GC标记的对象个数多8分之1，
  // 意味着在上一轮使用全部执行模式清除内存后，本轮GC触发时内存没有明显回弹增长，
  // 乐观地认为如果内存后续可以继续保持该节奏增长，起码还有好几轮才会再次重新触发全部执行模式。
  // 此时，分代式算法在执行完这些增量式算法的部分流程后，继续调用atomic2gen函数切换回分代式算法。
  // 清除所有未标记对象，重置内存基准值。
  // 最后一行setminordebt函数用于修正内存债务值，控制下轮GC触发时机
  if (newatomic < lastatomic + (lastatomic >> 3)) {  /* good collection? */
    atomic2gen(L, g);  /* return to generational mode */
    setminordebt(g);
  }
  else {  /* another bad collection; stay in incremental mode */
    // 若本轮标记的对象数量比起上一轮多于8分之1，则分代式算法悲观地认为在这种情况下，
    // 下次GC触发时定将很大可能会再次使用全部执行模式，此时算法采用的方案是后面一直保持使用完全不打断的增量式算法，
    // 让对象年龄更快速地进行成长，无引用的对象更快速地进行清除。虽然逻辑上也是同样使用增量式算法，
    // 但上述代码中最后一行对g->lastatomic的赋值，让它不为0，该值不为0的时候在每轮GC会依旧进入到分代式算法的逻辑中，
    // 然后下轮GC最终还是会回来上面这个stepgenfull函数，重复该流程，直到新一轮的完全增量式算法标记的数量小于8分之1，
    // 才会让分代式算法恢复回部分执行模式。
    g->GCestimate = gettotalbytes(g);  /* first estimate */
    entersweep(L);
    luaC_runtilstate(L, bitmask(GCSpause));  /* finish collection */
    setpause(g);
    g->lastatomic = newatomic;
  }
}


/*
** Does a generational "step".
** Usually, this means doing a minor collection and setting the debt to
** make another collection when memory grows 'genminormul'% larger.
**
** However, there are exceptions.  If memory grows 'genmajormul'%
** larger than it was at the end of the last major collection (kept
** in 'g->GCestimate'), the function does a major collection. At the
** end, it checks whether the major collection was able to free a
** decent amount of memory (at least half the growth in memory since
** previous major collection). If so, the collector keeps its state,
** and the next collection will probably be minor again. Otherwise,
** we have what we call a "bad collection". In that case, set the field
** 'g->lastatomic' to signal that fact, so that the next collection will
** go to 'stepgenfull'.
**
** 'GCdebt <= 0' means an explicit call to GC step with "size" zero;
** in that case, do a minor collection.
*/
static void genstep (lua_State *L, global_State *g) {
  if (g->lastatomic != 0)  /* last collection was a bad one? */
    stepgenfull(L, g);  /* do a full step */
  else {
    // 固定消费支出
    lu_mem majorbase = g->GCestimate;  /* memory after last major collection */
    // 新增消费检测值
    lu_mem majorinc = (majorbase / 100) * getgcparam(g->genmajormul);
    // 真实消费 > 固定消费 + 新增消费检测值，进行全代GC
    if (g->GCdebt > 0 && gettotalbytes(g) > majorbase + majorinc) {
      // 全代GC
      lu_mem numobjs = fullgen(L, g);  /* do a major collection */
      if (gettotalbytes(g) < majorbase + (majorinc / 2)) {
        /* collected at least half of memory growth since last major
           collection; keep doing minor collections. */
        // 若GC清理的内存不小于固定消费的一半，则仍然是像上述方案1中预充值较小的值，仍然使用真实消费金额的5分之1作为预充值金额；
        // 上面fullgen已经设置过下一次年轻代GC的时机，这里不需要再设置
        lua_assert(g->lastatomic == 0);
      }
      else {  /* bad collection */
        // 若GC清理的内存小于固定消费的一半，说明本次GC效果不佳，则会预充值更多的金额，
        // 让下轮GC更晚地到来，避免短时间内再去做一些无效的垃圾回收
        g->lastatomic = numobjs;  /* signal that last collection was bad */
        setpause(g);  /* do a long wait for next (major) collection */
      }
    }
    else {  /* regular case; do a minor collection */
      // 年轻代GC
      youngcollection(L, g);
      // 设置下一次年轻代GC的时机
      setminordebt(g);
      g->GCestimate = majorbase;  /* preserve base value */
    }
  }
  lua_assert(isdecGCmodegen(g));
}

/* }====================================================== */


/*
** {======================================================
** GC control
** =======================================================
*/


/*
** Enter first sweep phase.
** The call to 'sweeptolive' makes the pointer point to an object
** inside the list (instead of to the header), so that the real sweep do
** not need to skip objects created between "now" and the start of the
** real sweep.
*/
// 把GC阶段状态值由GCSenteratomic切换到下一个阶段GCSwpallgc以外，
// 还通过sweeptolive函数的返回值来初始化我们上述的这个g->sweepgc迭代器指针，
// 表示后面清除会从g->sweepgc指针指向的这个位置开始
static void entersweep (lua_State *L) {
  global_State *g = G(L);
  g->gcstate = GCSswpallgc;
  lua_assert(g->sweepgc == NULL);
  g->sweepgc = sweeptolive(L, &g->allgc);
}


/*
** Delete all objects in list 'p' until (but not including) object
** 'limit'.
*/
static void deletelist (lua_State *L, GCObject *p, GCObject *limit) {
  while (p != limit) {
    GCObject *next = p->next;
    freeobj(L, p);
    p = next;
  }
}


/*
** Call all finalizers of the objects in the given Lua state, and
** then free all objects, except for the main thread.
*/
void luaC_freeallobjects (lua_State *L) {
  global_State *g = G(L);
  g->gcstp = GCSTPCLS;  /* no extra finalizers after here */
  luaC_changemode(L, KGC_INC);
  separatetobefnz(g, 1);  /* separate all objects with finalizers */
  lua_assert(g->finobj == NULL);
  callallpendingfinalizers(L);
  deletelist(L, g->allgc, obj2gco(g->mainthread));
  lua_assert(g->finobj == NULL);  /* no new finalizers */
  deletelist(L, g->fixedgc, NULL);  /* collect fixed objects */
  lua_assert(g->strt.nuse == 0);
}


// 原子阶段
// 这个阶段能解决的增量式算法带来的两个问题：
// 1）特殊灰色链表需求：Table/UserData在使用后退屏障保证标记阶段一致性原则时，若存在频繁大量数据改变，
// 并不能直接插入到标记传播gray灰色链表中，因为这样会潜在风险，若标记传播阶段新增任务量大于能处理的任务量，
// 会导致gray链表永远处理不完，所以需要有另一个特殊的灰色链表存储这些使用后退屏障的对象，且该链表不能影响标记传播阶段。
// 2）等待标记结束需求：Key_WeakTable键弱引用表需要一个阶段去等待其它对象处理完毕才能启动它的值标记流程，
// 因为在键未被标记的情况下Table自身是不允许标记它的值的。
static lu_mem atomic (lua_State *L) {
  // 在标记过程中若这几个标记根结点对象被修改或替换，变成了白色的对象，也并没有违反一致性原则，
  // 因为一致性原则保证的是没有黑色对象引用白色对象，既然如此，Lua也无法使用屏障去修复这种根结点对象被修改颜色异常的问题
  // 这里需要重新扫描
  global_State *g = G(L);
  lu_mem work = 0;
  GCObject *origweak, *origall;
  GCObject *grayagain = g->grayagain;  /* save original list */
  g->grayagain = NULL;
  lua_assert(g->ephemeron == NULL && g->weak == NULL);
  lua_assert(!iswhite(g->mainthread));
  g->gcstate = GCSatomic;
  markobject(g, L);  /* mark running thread */
  /* registry and global metatables may be changed by API */
  markvalue(g, &g->l_registry);
  markmt(g);  /* mark global metatables */
  work += propagateall(g);  /* empties 'gray' list */
  /* remark occasional upvalues of (maybe) dead threads */
  work += remarkupvals(g);
  work += propagateall(g);  /* propagate changes */
  // 直接先把grayagain链表直接赋值给gray链表，然后对这个赋值后的gray链表执行标记传播，
  // 这样就能确保使用了后退屏障插入到grayagain链表的这些对象都能得到正确的标记了
  g->gray = grayagain;
  work += propagateall(g);  /* traverse 'grayagain' list */
  // 集中处理键弱引用表
  convergeephemerons(g);
  // convergeephemerons阶段结束后，所有Table的标记均已完成，通常来说，
  // 此时未标记的key或value就是后面清除阶段需要清除的垃圾对象。
  // 但是对于Table类型来说，它在数据清除上有个特性：
  // 就是当它的元素（键值对）某个value被清空时，它的key也需要被清空，从而可以达到删除某个元素的作用。
  // 
  // 之所以只需要处理清空g->weak和g->allweak中的key，是因为Table只有在这两种值弱引用关系中，
  // Table的value才有可能出现未标记而对应的key又是已标记状态，Key_WeakTable所在的g->ephemeron链表是不存在这种情况的，
  // 所以只有它们两个链表才需要去根据value去清空key。
  /* at this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers */
  clearbyvalues(g, g->weak, NULL);
  clearbyvalues(g, g->allweak, NULL);
  // 原子阶段下一部分就是把需要释放的带FINALIZEDBIT标记的对象从g->finobj链表移入到g->tobefnz链表，
  // 并对g->tobefnz链表对象全部进行标记
  origweak = g->weak; origall = g->allweak;
  separatetobefnz(g, 0);  /* separate objects to be finalized */
  work += markbeingfnz(g);  /* mark objects that will be finalized */
  work += propagateall(g);  /* remark, to propagate 'resurrection' */
  // 因为析构器g->tobefnz链表中的对象被全部标记了，在这个标记与标记传播的过程中，可能会遍历到新的弱引用关系表，
  // 这些表也将同样地被按类型放入到g->ephemeron，g->weak，g->allweak链表中。
  // 所以，在析构器标记传播阶段结束后，我们需要重新处理这部分的代码。
  // 并把Table中key或者value为空的键值对元素都标记为无效待清除状态
  convergeephemerons(g);
  /* at this point, all resurrected objects are marked. */
  /* remove dead objects from weak tables */
  // 其中再次使用了clearbyvalues函数，但这里与之前使用不同的是这次第3个参数不为空了，
  // 该参数代表的是要从哪个指针位置开始处理链表。上图中传入origweak和origall作为第3个参数，
  // 意思是从标记析构器流程开始统计，从那之后新添加的Value_WeakTable和KeyValue_WeakTable才是需要重新处理标记的对象，
  // 因为在这之前的那些Table已经是处理过了，不需要再次处理。
  clearbykeys(g, g->ephemeron);  /* clear keys from all ephemeron tables */
  clearbykeys(g, g->allweak);  /* clear keys from all 'allweak' tables */
  /* clear values from resurrected weak tables */
  clearbyvalues(g, g->weak, origweak);
  clearbyvalues(g, g->allweak, origall);
  // 原子阶段会清空字符串缓存g->strcache，这个缓存是用于提高字符串访问的命中率的
  luaS_clearcache(g);
  // 在原子阶段的最后，会把current_white与other_white互换
  g->currentwhite = cast_byte(otherwhite(g));  /* flip current white */
  lua_assert(g->gray == NULL);
  return work;  /* estimate of slots marked by 'atomic' */
}


// 增量式算法中清除阶段中的一步清除操作，该函数第3，第4个参数分别代表处理完之后要
// 切换到的下一个阶段的枚举值和下一个阶段将要被清除的链表
// 
// 该函数通过if条件语句把逻辑分成两种情况：
// 1) 该函数内部就会使用到了我们上面说的重要字段g->sweepgc，在上个阶段中它指向allgc链表中的元素。
// 在本处的逻辑是，若g->sweepgc指向的对象不为空，则调用sweeplist函数继续对g->sweepgc指向的链表继续进行处理。
// 这里函数第3个参数countint为GCSWEEPMAX常量，它的值为100，代表增量式清除阶段每轮允许处理或清除的最大对象个数为100个
// 2）g->sweepgc指向的链表在每轮sweepstep中最多可处理100个对象，在一定轮次过后，链表中的对象会全部处理完毕，
// 此时g->sweepgc指向链表末端的下一个元素，即空指针。这时候逻辑会走到另一个分支，
// sweepstep会修改GC阶段为下一个阶段，并把g->sweepgc这个迭代器指针指向下一个需要被处理的链表。
static int sweepstep (lua_State *L, global_State *g,
                      int nextstate, GCObject **nextlist) {
  if (g->sweepgc) {
    l_mem olddebt = g->GCdebt;
    int count;
    g->sweepgc = sweeplist(L, g->sweepgc, GCSWEEPMAX, &count);
    g->GCestimate += g->GCdebt - olddebt;  /* update estimate */
    return count;
  }
  else {  /* enter next state */
    g->gcstate = nextstate;
    g->sweepgc = nextlist;
    return 0;  /* no work done */
  }
}


// GC算法入口
static lu_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  lu_mem work;
  lua_assert(!g->gcstopem);  /* collector is not reentrant */
  g->gcstopem = 1;  /* no emergency collections while collecting */
  switch (g->gcstate) {
    case GCSpause: {
      // 标记起始阶段
      restartcollection(g);
      g->gcstate = GCSpropagate;
      work = 1;
      break;
    }
    case GCSpropagate: {
      // 标记传播阶段
      if (g->gray == NULL) {  /* no more gray objects? */
        g->gcstate = GCSenteratomic;  /* finish propagate phase */
        work = 0;
      }
      else
        work = propagatemark(g);  /* traverse one gray object */
      break;
    }
    case GCSenteratomic: {
      // 标记原子阶段
      work = atomic(L);  /* work is what was traversed by 'atomic' */
      // 扫描标记结束，进入清除阶段
      entersweep(L);
      g->GCestimate = gettotalbytes(g);  /* first estimate */
      break;
    }
    case GCSswpallgc: {  /* sweep "regular" objects */
      // 在g->allgc链表中找到了一个未标记的对象，也设置完了g->sweepgc，
      // 接下来就看标记清除算法的下一部分：allgc链表清除阶段。该子阶段枚举值为“GCSswpallgc”，
      // 全称“Garbage Collect State sweep allgc”，即“垃圾回收清除allgc链表”阶段
      work = sweepstep(L, g, GCSswpfinobj, &g->finobj);
      break;
    }
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      // 1）所有带有未触发的析构器的GCObject对象在创建的时候并不是存储于g->allgc链表中的，而是会存储到g->finobj链表中；
      // 2）当g->finobj中的对象因为未标记将要被释放时，会在原子阶段把它们从g->finobj链表移出，并移入到g->tobefnz链表中，
      // 并在原子阶段中对g->tobefnz链表的所有的元素进行标记，以确保它们在GC最后的析构器执行阶段之前，不会因未标记而在清除阶段被清除。
      // 3）析构器对象的回收拆分在2轮完整GC流程中，在第一轮GC析构器执行之后，会把他们从g->tobefnz链表移出，并删除其析构器标记，
      // 把对象当作一个无析构器对象重新插入到g->allgc链表中。在第二轮完整GC中，若对象仍然没有被标记，
      // 则只需要把它当作普通的对象进行正常的删除与内存回收即可。
      work = sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
      break;
    }
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      // 清除阶段单步清除操作sweepstep函数，内部又调用了sweeplist。该函数除了清除未标记对象以外，还负责重置该对象的颜色标记。
      // g->tobefnz链表中的对象确实在原子阶段被标记了，所以他们都是黑色对象，这里需要把他们颜色重置为当前白色。
      work = sweepstep(L, g, GCSswpend, NULL);
      break;
    }
    case GCSswpend: {  /* finish sweeps */
      // CSswpfinobj与GCSswptobefnz阶段结束后，对析构器相关的g->finobj链表与g->tobefnz链表都完成了清除与颜色重置操作。
      // 接下来就进入到“清除结束”阶段
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      work = 0;
      break;
    }
    case GCScallfin: {  /* call remaining finalizers */
      // 在原子阶段中会把将要被释放的带析构器的对象插入到g->tobefnz链表中，并对该g->tobefnz链表的所有对象进行标记以防止清除阶段被清除。
      // 到了GCScallfin这一个阶段，就是要对g->tobefnz链表元素进行处理了，该阶段调用了runafewfinalizers函数，执行一些析构器。
      // 调用此函数的参数GCFINMAX为常量10，代表每步GC最大执行10个析构器函数
      // 每一个析构器函数的执行工作量的评估为GCFINALIZECOST，它是常量50，意思是对于GC而言，执行一次析构器的性能花销预估，
      // 大概会跟遍历50个对象一样，
      if (g->tobefnz && !g->gcemergency) {
        g->gcstopem = 0;  /* ok collections during finalizers */
        work = runafewfinalizers(L, GCFINMAX) * GCFINALIZECOST;
      }
      else {  /* emergency mode or no more finalizers */
        // 当全部析构器执行完毕后，GC就会重新恢复到初始的GCSpause阶段，等待下一次GC的触发
        g->gcstate = GCSpause;  /* finish collection */
        work = 0;
      }
      break;
    }
    default: lua_assert(0); return 0;
  }
  g->gcstopem = 0;
  // 返回了这次GC步骤对应的工作量
  return work;
}


/*
** advances the garbage collector until it reaches a state allowed
** by 'statemask'
*/
void luaC_runtilstate (lua_State *L, int statesmask) {
  global_State *g = G(L);
  while (!testbit(statesmask, g->gcstate))
    singlestep(L);
}



/*
** Performs a basic incremental step. The debt and step size are
** converted from bytes to "units of work"; then the function loops
** running single steps until adding that many units of work or
** finishing a cycle (pause state). Finally, it sets the debt that
** controls when next step will be performed.
*/
// 步进式GC
// 算法每次触发后，在开始算法真正开始之前，会根据当前欠下的债务以及需要预充值的金额，
// 进行一个工作量的评估，然后通过while循环不断地去工作，直到完成这些工作量，才允许结束本轮GC
static void incstep (lua_State *L, global_State *g) {
  // 步进倍率 |1 操作是为了避免下面除0操作
  int stepmul = (getgcparam(g->gcstepmul) | 1);  /* avoid division by 0 */
  // 没有利用global_State中的g->GCdebt（真实负债），是因为如果用了g->GCdebt来判断，
  // 而本轮GC没能够成功释放到足够内存的时候，标记清除算法就会进入死循环，
  // 或者执行了过多的工作才结束而极大影响性能
  // 因此标记清除算法没有直接使用g->GCdebt，而是在开始的时候把它先用自定义的公式转化为工作量，
  // 然后把每轮GC流程拆分成一步步独立的工作singlestep，每步工作累积一定工作量，当工作量积累足够了就停止本轮GC，
  // 而不需要完成完整GC中所有的流程，这就是增量式标记清除算法之所以称之为增量式（可分步）而不是全量式（不可打断）的原因
  // 
  // 工作量 debt = (抵扣完预充值部分的新对象分配的堆内存 / sizeof(TValue)) * 100
  l_mem debt = (g->GCdebt / WORK2MEM) * stepmul;
  // 需要向系统预充值的金额
  // 之所以还清债务后还要预充值一笔金额，是因为这样可以做一个债务缓冲，下次债务增长时可以先从预充值的金额扣除，
  // 可以避免债务迅速大于0而再次快速触发GC，能有效控制GC的频率
  // 简单来说标记清除算法对债务的清算：就是处理工作的过程，然后在工作量完成后把全局的负债g->GCdebt设置为那个预充值的金额
  // 
  // 工作的结束条件为向系统预充值的工作量，数值至少为：
  // stepsize = （8KB / sizeof(TValue)) * 100
  // 即处理800KB对应的TValue的个数。
  //
  // 由debt和stepsize这两条公式可知，如果设备性能好，想要每次单步垃圾回收过程（singlestep工作）处理更多的对象，
  // 即允许完成更多的工作，可以调大stepmul的数值。意味着每次标记清除算法触发后，需要完成更多的工作和预充值更多的金额，
  // 即需要处理更多的TValue对象个数，才能结束本轮垃圾回收过程。
  l_mem stepsize = (g->gcstepsize <= log2maxs(l_mem))
                 ? ((cast(l_mem, 1) << g->gcstepsize) / WORK2MEM) * stepmul
                 : MAX_LMEM;  /* overflow; keep maximum value */
  do {  /* repeat until pause or enough "credit" (negative debt) */
    lu_mem work = singlestep(L);  /* perform one single step */
    debt -= work;
  } while (debt > -stepsize && g->gcstate != GCSpause);
  if (g->gcstate == GCSpause)
    setpause(g);  /* pause until next cycle */
  else {
    // 和上面的算法相反，从work units到字节数
    debt = (debt / stepmul) * WORK2MEM;  /* convert 'work units' to bytes */
    luaE_setdebt(g, debt);
  }
}

/*
** Performs a basic GC step if collector is running. (If collector is
** not running, set a reasonable debt to avoid it being called at
** every single check.)
*/
void luaC_step (lua_State *L) {
  global_State *g = G(L);
  if (!gcrunning(g))  /* not running? */
    luaE_setdebt(g, -2000);
  else {
    if(isdecGCmodegen(g))
      // 分代GC
      genstep(L, g);
    else
      // 步进式GC
      incstep(L, g);
  }
}


/*
** Perform a full collection in incremental mode.
** Before running the collection, check 'keepinvariant'; if it is true,
** there may be some objects marked as black, so the collector has
** to sweep all objects to turn them back to white (as white has not
** changed, nothing will be collected).
*/
static void fullinc (lua_State *L, global_State *g) {
  if (keepinvariant(g))  /* black objects? */
    entersweep(L); /* sweep everything to turn them back to white */
  /* finish any pending sweep phase to start a new cycle */
  luaC_runtilstate(L, bitmask(GCSpause));
  luaC_runtilstate(L, bitmask(GCSpropagate));  /* start new cycle */
  g->gcstate = GCSenteratomic;  /* go straight to atomic phase */
  luaC_runtilstate(L, bitmask(GCScallfin));  /* run up to finalizers */
  /* estimate must be correct after a full GC cycle */
  lua_assert(g->GCestimate == gettotalbytes(g));
  luaC_runtilstate(L, bitmask(GCSpause));  /* finish collection */
  setpause(g);
}


/*
** Performs a full GC cycle; if 'isemergency', set a flag to avoid
** some operations which could change the interpreter state in some
** unexpected ways (running finalizers and shrinking some structures).
*/
void luaC_fullgc (lua_State *L, int isemergency) {
  global_State *g = G(L);
  lua_assert(!g->gcemergency);
  g->gcemergency = isemergency;  /* set flag */
  if (g->gckind == KGC_INC)
    fullinc(L, g);
  else
    fullgen(L, g);
  g->gcemergency = 0;
}

/* }====================================================== */


