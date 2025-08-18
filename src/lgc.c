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
// ����WORK2MEMû��ʹ��GCObject�Ĵ�С������ȡ��TValue�Ĵ�С
// ��Ϊ�������㷨�����Ĺ�����Ҫ����TValue����Ϊ��λ������������ǣ�����Ȳ�������Щ������������������������
// �����п��ܶ��TValue�������϶�Ӧ��ͬһ��GCObject�����⹤����Ҳ�������������Ƿ����ͷ�GCObject�����ڴ��޹أ�
// �Ƿ����ȫ����ʵծ��g->GCdebt�޹أ�����ʹ��TValueΪ��λ��
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
// ����Ϊ��ɫ
#define set2gray(x)	resetbits(x->marked, maskcolors)


/* make an object black (coming from any color) */
// ����Ϊ��ɫ
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
// ǰ������
// Lua�ڴ󲿷�����¶���ʹ��ǰ�����ϣ������̰Ѹð�ɫ������б�ǣ�ֻ���ڴ���Table��UserDataʱʹ�ú�������
void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
  global_State *g = G(L);
  lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
  if (keepinvariant(g)) {  /* must keep invariant? */
    // ��ǽ׶Σ���Ըñ���ɫ�������õİ�ɫ�������̽��б�ǣ��޸�������ɫΪ��ɫ���ɫ
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
// ��������
void luaC_barrierback_ (lua_State *L, GCObject *o) {
  global_State *g = G(L);
  lua_assert(isblack(o) && !isdead(g, o));
  lua_assert((g->gckind == KGC_GEN) == (isold(o) && getage(o) != G_TOUCHED1));
  // �ѱ�ǽ׶����������һ�������°����øð�ɫ����ĺ�ɫ�����޸Ļػ�ɫ
  if (getage(o) == G_TOUCHED2)  /* already in gray list? */
    set2gray(o);  /* make it gray to become touched1 */
  else  /* link it in 'grayagain' and paint it gray */
    linkobjgclist(o, g->grayagain);
  if (isold(o))  /* generational mode? */
    setage(o, G_TOUCHED1);  /* touched in current cycle */
}


// ����Ҫ��פ���ڴ����GCObject�����allgc�������Ƴ���
// �����뵽ͬ��������global_State�е�fixedgc�����н����������
// ����������׶ξͲ��ᱻ�������ˣ��Ͳ�����һЩ���������ж���
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
// Ⱦɫ����
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
// ��ǽ׶εĵ�һ��
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
// ֵ���������ã���������ǿ���ã�����ֻ��Ҫ��Ǽ���������Ҫ���ֵ
static void traverseweakvalue (global_State *g, Table *h) {
  Node *n, *limit = gnodelast(h);
  /* if there is array part, assume it may have white values (it is not
     worth traversing it now just to check) */
  int hasclears = (h->alimit > 0);
  for (n = gnode(h, 0); n < limit; n++) {  /* traverse hash part */
    // ��ֵ����պ󣬶�Ӧ�ļ������Ƿ񱻱�ǣ����ᱻ���
    if (isempty(gval(n)))  /* entry is empty? */
      clearkey(n);  /* clear its key */
    else {
      lua_assert(!keyisnil(n));
      // ֻ��Ҫ��Ǽ�������Ҫ���ֵ
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
// ��������������ã���ֵ����ǿ���ã�����ֻ��Ҫ���ֵ��������Ҫ��Ǽ�
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


// ����table��������пɴ�Ľ��
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


// ����table��������пɴ�Ľ��
static lu_mem traversetable (global_State *g, Table *h) {
  const char *weakkey, *weakvalue;
  const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
  TString *smode;
  markobjectN(g, h->metatable);
  // 1)"k": ������Ϊ�����ã��������ÿ�ʱ��table�����������
  // 2)"v": ����ֵΪ�����ã���ֵ���ÿ�ʱ��table�����������
  // 3)"kv": ��������ֵ��Ϊ�����ã�������һ�����ÿ�ʱ��table�����������
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
      // �������ֵ��Ϊ�����ã�ֱ�Ӱѵ�ǰ���table���ӵ���gc�������յ������У�������û������������������ڵ�Ԫ�أ��򽫱�ȫ�����
      linkgclist(h, g->allweak);  /* nothing to traverse now */
  }
  else  /* not weak */
    // ˵��û����__mode������ǿ����
    traversestrongtable(g, h);
  return 1 + h->alimit + 2 * allocsizenode(h);
}


// ����full user data��������пɴ�Ľ��
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
// �����������ͣ�������пɴ�Ľ��
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


// ����C�հ���������пɴ�Ľ��
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
// ����lua�հ���������пɴ�Ľ��
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
// �����̣߳�Э�̣���������пɴ�Ľ��
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
// ��ɫ����
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
// ���д���������ñ�
// ��ԭ�ӽ׶α����ֿ�ʼ��ʱ�����е�Key_WeakTable���Ѿ���gray����grayagain�б��Ƴ�������һЩ��ǣ�
// ������Ϻ������Ǵ洢��g->ephemeron����Ϊ���ݵ���������У�����������ʱ����Ϊ��E������
// convergeephemerons�����Ĺ��ܾ��Ǵ�������洢��Key_WeakTable��E���������߼������¼�����
// 1��ȡ����whileѭ�����ִν��д���ÿ����ѭ���ȴ�E����ȡ��ȫ����Key_WeakTable��
// ����һ����ʱָ��ָ������ʾ�ȴ��������ž����g->ephemeronԭ����ָ�룻
// 2������������ѭ����ĳ��˳��Եȴ������ԭE�����е�Key_WeakTable���δ���
// ����ʱ������ĳ��Key_WeakTable��Ȼ��������һ��Key��Value��Ϊ�յĽ�㣬��Ѹ�Table���²��뵽��һ�ֵ�E�����У�
// ����������һ�������ù��Ѿ�Ϊ������
// 3������ѭ��������һ����ѭ������������Key_WeakTable����ȫ������������������������ɫ��Ǳ��޸ģ�����ѭ��while���¼�����ʼ��һ�֣�
// �ظ���һ�����������ﻹ�и���dir������������Ϊ!dir������Ż�ϸ��
// 4���ޱ�Ǹı䣺��һ����ѭ��������������������Ƿ����ı�ʱ��convergeephemerons����������
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
// ��Table����δ����ǵ�value����Ѹ�value��Ӧ��keyҲ����Ϊ��Ч�����״̬���������keyֵ�����ڱ�ǽ׶α������
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


// ���ݶ�������͵��ò�ͬ���ͷź���
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
// �������ͨ������������������˳����б��������ڱ���ÿһ��Ԫ�ص�ʱ�������²�����
// 1������ǰ����������Ԫ��δ��ǣ�����ɫΪother_white������ö���������������Ҫ�������
// ���ǻ����freeobj�����ͷŸö�����ڴ棬��������ָ����ǰ����
// freeobj��������ݶ�������͵��ò�ͬ���ͷź�����
// ��֤ÿ�ֶ�������ȷ�ش����ͷ��������õ������Ӷ���
// 2������ǰԪ���Ѿ�������ˣ���ʱ������ɫ������current_white��ǰ�ף�ԭ�ӽ׶κ󴴽��Ķ���
// ���ߺ�ɫ����ǽ׶α�������ɣ�����Ԫ��Ϊ�Ϸ�����Ԫ�ػ����������
// ����������ɫ�������Ϊcurrent_white��ǰ�ף�������ɺ�����ͷ��ָ�벻�䣬
// ѭ��������ָ�������ǰ����ָ����һ�����󡣡�
// 
// ������
// 1��lua_State * L������ǰ���иú������̡߳�����ÿ��������һ��������������
// ��Ϊ�ܶຯ����������Ҫ��״̬���Ǵ洢���̻߳���global_State�С�
// 2��GCObject **p��ָ��GCObject * ��ָ�룬����������Ϊ������һ������
// �����е�Ԫ�ؿ���ͨ��nextָ���ֶ�ָ����һ��Ԫ�ء�
// 3��int countin������ú��������������������������л���һ���������Ԫ�أ�
// ÿ������������һ�������ʱ�򣬼�����1���������ﵽcountinʱ��ѭ��������
// �����ķ���ֵΪ��һ��Ҫ����Ķ����ָ�롣
// 4��int countout������ú���ִ����Ϻ�ʵ�ʴ����˵Ķ����������������������㹻�࣬
// ��countout��countin��ȣ�������������δ�ﵽcountin������ʱ��͵���������ĩ�ˣ�
// ��ʱcountout��С��countin��
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
// sweeptolive���������߼����£�
// 1����ÿ��whileѭ�������У�sweeplist�ᱻ����һ�Σ�����ʱ�����ֵ�ǰ�����һ������δ����ǣ�
// �������������������g->allgc����ָ�������������ָ��ͬʱ�����ƶ���
// ������ʼ��ʱ�������ָ����ָ��g->allgc����ͷ����Ȼ����Ϊ������ͬʱ�ƶ�������������Ȼ����ָ����ͬһ������
// ����sweeplist������sweeptolive��whileѭ��������p == old���������㣬�������һ��ѭ���Ĵ���
// 2������ǰ�����һ������Ϊ�ѱ�Ƕ�����ִ�������g->allgc����ָ��ά�ֲ��䣬��ָ��ԭ���Ķ���
// ��������������ָ��������ǰ��������ָ��g->allgc�������һ���������Դ�ʱsweeplist�������غ�
// whileѭ��������p == old�����������㣬��Ϊһ��ָ����Ȼָ��g->allgc����ͷ����
// һ��ָ��ָ�����ͷ������һ������ѭ��������
// ���գ�sweeptolive����ִ����Ϸ��غ󣬴�ʱ��״̬�ǣ�g->sweepgcָ���ָ��g->allgc��������һ����Ҫ����Ķ���
// ��g->allgc�����ͷ��һ������Ϊһ���ѱ�ǵĺϷ�����
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
// ������ܣ������ַ�����
// ��һ�����ַ���û�б������������ʱ����������׶α������ͬʱ����strt�е�ָ��Ҳ������Ϊ��ָ�룬
// ÿ�Ƴ�1�����ַ�������strt�ľ�nuse���1������ʱstrt�Ѿ�����õ��ڴ�ռ���ʵ�ǲ�����ô��������Զ����ż�С�ġ�
// checkSizes���������ã����Ǽ��nuse�Ĵ�С�������Ѿ���С�������ϣ������size��4��֮1�ˣ������ʱ��ϣ�������ʽϵͣ�
// �����ռ�û�б��õ������Թ�ϣ���ڴ�������·����������ÿ�λ����������ԭ����2��֮1�������Ϳ��Խ�ʡ��ԭ��һ����ڴ�ռ䡣
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


// __gcԪ����
// �ڶ���Ҫ�����ٵ�ʱ����ã�һ������ڿ���һЩ��Դ���ͷ�
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
// ����ִ��һ��������g->tobefnz�����е�Ԫ�ص������������е��õ�GCTM������
// ��Ϊִ��__gcԪ����
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
// Ԥ��ֵ��� = 2 * �̶����� - ��ʵ����
// �����ǰ�Ĺ̶�����Ϊ100���������Ѽ��ֵĬ��������ȣ�����ҲΪ100����GC��Ϻ���ʵ����ʣ��Ϊ160��
// ���Դ�ʱ��ҪԤ��ֵ�Ľ�� = 2 * 100 - 160 = 40��
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
// ���������е������������δ��ǵĶ����ѱ�ǵĶ���������ǵ�������ΪG_OLD�϶���
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
// ��������ĳ�������ڵĶ��󣬲���ȥ�޸Ķ������ɫ��ִ�����²�����
// 3.1�����δ��ǵĶ���
// 3.2���ѱ�ǵĶ������������ĳɳ���
static GCObject **sweepgen (lua_State *L, global_State *g, GCObject **p,
                            GCObject *limit, GCObject **pfirstold1) {
  // �ɳ���ϵ
  // ���Ŀ�����䣬�ұ�ԭʼ����
  static const lu_byte nextage[] = {
    G_SURVIVAL,  /* from G_NEW */
    G_OLD1,      /* from G_SURVIVAL */
    G_OLD1,      /* from G_OLD0 */
    G_OLD,       /* from G_OLD1 */
    G_OLD,       /* from G_OLD (do not change) */
    G_TOUCHED1,  /* from G_TOUCHED1 (do not change) */
    G_TOUCHED2   /* from G_TOUCHED2 (do not change) */
  };
  // 1��G_NEW->G_SURVIVAL��
  // 2��G_SURVIVAL, G_OLD0->G_OLD1��
  // 3��G_OLD1->G_OLD��
  // 4��G_TOUCHED1, G_TOUCHED2����G_OLD0��G_OLD1�ԣ����������������ڱ�ǽ׶λ�ԭ�ӽ׶α�����ʱ���������ɳ���
  // ����sweepgen�����гɳ�����������ÿ��GC��ִ���Զ��ɳ�Ϊ�϶��󣬳�����������������Ϊ��������䣻
  // 5��G_OLD������һ���ˣ����䲻��Ҫ�ٳɳ���
  // ���⣬ͨ���ú������һ������ * *pfirstold1����Ҳ����֪����sweepgen��������������������ɳ����⣬
  // �������ҵ���һ��G_OLD1�����Ԫ�أ���ֵ����
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
// �ú���������պ�����һЩ�ִ�ʽ�㷨���õ������ݽṹ
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
// ��from��to������ڵ�ΪG_OLD1����Ϊ��ɫ�������mark�����Ǻ�ɫ�������ýڵ��Ѿ���mark�����Բ�����mark
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
// ��ÿ�ηִ�ʽ�㷨����ִ��ģʽִ�н��������
// 1������cleargraylist�����������һЩGC״̬���ݣ�
// 2������checkSizes�������ж϶��ַ�������stringtable�Ƿ���Ҫ�ͷţ�
// 3��ִ�е��ֱ�����Ķ������������
static void finishgencycle (lua_State *L, global_State *g) {
  // �ú���������պ�����һЩ�ִ�ʽ�㷨���õ������ݽṹ
  correctgraylists(g);
  // ��⵱ǰ�Ķ��ַ�������stringtable��ʹ���ʲ���4��֮1������С���ͷ�һ����������
  checkSizes(L, g);
  // ���õ�ǰgc�׶�ΪGCSpropagate������û�����ý׶�Ϊ�ĵ�һ��GC�׶�GCSpause��
  // ������������ֱ������ΪGCSpause����һ���׶�GCSpropagate��������Ϊ����֪��GCSpause�ڽ���GCSpropagate�׶�ǰ��Ҫ���������ǣ�
  // ��global_State�������б�ǣ�Ȼ��ѱ�ǽ��д�����
  // ���ִ�ʽ�㷨�Ĳ���ִ��ģʽ�ڱ�ǽ׶�ÿ��ֻ��Ҫ��G_OLD1��һ������ı��ͨ�����ù�ϵ�������������õ�����һ������
  // ��Щ��һ���������������Ϊ��Ǵ�������ʼ���ˣ����Կ�������global_State��صĸ���������̣�������Ҫʹ������ĸ�����ˡ�
  g->gcstate = GCSpropagate;  /* skip restart */
  if (!g->gcemergency)
    callallpendingfinalizers(L);
}


/*
** Does a young collection. First, mark 'OLD1' objects. Then does the
** atomic step. Then, sweep all lists and advance pointers. Finally,
** finish the collection.
*/
// �ִ�GC����ִ��
// �ռ�����һ�������ִ�ʽGC����ʱ�ڴ���������������׼ֵʱ��Ĭ��Ϊ100% ��������GC�ͻ�ʹ�ò���ִ��ģʽ
// ʹ�÷ִ�ʽ�㷨��ʱ��Ӧ�þ������ֳ���ʹ�ò���ִ��ģʽ����Ϊֻ���ڸ�ģʽ�£��Ų���Ҫ����ȫ���Ķ���
// ��������ʽ�������㷨�ž������ơ�
static void youngcollection (lua_State *L, global_State *g) {
  // ����ִ��ģʽҲ�ֱ��������������֣��öδ�����Ǳ�ǽ׶Σ���ǽ׶εĺ��ĺ�������markold������
  // �ú����������Ǳ���һ����������Ķ��󣬰���������ΪG_OLD1�Ķ���ɳ�ΪG_OLD���󣬲���Ǹö�����ӽ�㡣
  // G_OLD1����Ķ���������һ������������GC�㷨�����ڸ����ɴ����Ч���󣬵����ǵ�ǰ���ܻ���������������һ����ɫ����
  // �����ڱ���GC�������ȴ���Щ�����ӽ������һ�ֱ���������Ϻ����ǾͿ��Գɳ�Ϊ����һ��G_OLD������󣬼�����������һ������
  // ������������δ��ǵ��������Ҳ������Ҫ�����κα��������Ĺ�����
  //  ��ǽ׶β���Ҫ�������еĶ���ֻ��Ҫ����G_OLD1���󲢱�����ǵ��ӽ�㣬Ҳ����Ϊ����Ҫ����ȫ������
  // ���Էִ�ʽ�㷨����ִ��ģʽ�����ܽϸߣ�
  GCObject **psurvival;  /* to point to first non-dead survival object */
  GCObject *dummy;  /* dummy out parameter to 'sweepgen' */
  lua_assert(g->gcstate == GCSpropagate);
  if (g->firstold1) {  /* are there regular OLD1 objects? */
    // allgc����[firstold1, reallyold)���䡣
    // reallyoldָ�����Ķ���Ϊ����һ�����������ΪG_OLD��
    // ��firstold1ָ�룬��������һ��GC������ɺ󻺴��ָ��allgc�����еĵ�һ��G_OLD1��������ָ�롣
    markold(g, g->firstold1, g->reallyold);  /* mark them */
    g->firstold1 = NULL;  /* no more OLD1 objects (for now) */
  }
  // finobj����[finobj, finobjrold]�����������Ķ���Ԫ���������ޣ�����û�в������Ƶ�����firstold1ָ����ʽ��
  // ��Լһ��ָ�룬ֱ�ӱ���������finobj����ͷ������һ��finobjrold��ʼλ������������ж���
  markold(g, g->finobj, g->finobjrold);
  // tobefnz����������������GC���������������ͷŵĶ��󣬴�finobj����������Ķ�����finobj�����Ǵ����߼�ͬ��
  markold(g, g->tobefnz, NULL);
  // �ò���Ϊ�ִ�ʽ�㷨��ԭ�ӱ�ǽ׶Ρ�
  // �ú�����ʹ����������㷨������ӵ�gray��ɫ�����еĶ��󣬶��������õ��ӽ��Ҳ���б�ǣ�ԭ�ӽ׶�ͬʱҲ�������Ϻ������ù�ϵ������⡣
  // atomic����������Ϻ�����ͨ��G_OLD1�϶������õ�������󶼵ݹ������˱�ǡ�
  // ��������������g->gcstate�׶�Ϊ����׶�GCSswpallgc��׼����ʼGC�Ķ������������
  atomic(L);

  /* sweep nursery and get a pointer to its last live element */
  g->gcstate = GCSswpallgc;
  // �ò��ִ���ദ������sweepgen�������ú����������ǣ���������ĳ�������ڵĶ���ִ�����²�����
  // 3.1�����δ��ǵĶ���
  // 3.2���ѱ�ǵĶ������������ĳɳ���
  // ������Ҫע����ǣ�sweepgen��������ȥ�޸Ķ������ɫ
  psurvival = sweepgen(L, g, &g->allgc, g->survival, &g->firstold1);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->old1, &g->firstold1);
  // ����ִ����Ϻ�allgc�����Ԫ�ر�sweepgen������������ˣ����������Ҳ�ɳ��ˣ�
  // ���ŵĶ����ֳɹ�������һ��GC����������ͨ�����´�����ָ�����¸�ֵ����allgc�ڸ�����������ǰ�ƣ�����������������λ�����¶�Ӧ��
  // ��Ϊ�����������䶼�ɳ�Ϊ����һ�׶ε�������ϵĶ����ˣ�
  g->reallyold = g->old1;
  g->old1 = *psurvival;  /* 'survival' survivals are old now */
  g->survival = g->allgc;  /* all news are survivals */

  /* repeat for 'finobj' lists */
  // ���������������׶Ρ�ͬ������finobj������������������allgc������һ�£�Ҳ�����δ��Ƕ��󣬳ɳ��ѱ�Ƕ��󣬲��������ڵ�������ǰ��
  dummy = NULL;  /* no 'firstold1' optimization for 'finobj' lists */
  psurvival = sweepgen(L, g, &g->finobj, g->finobjsur, &dummy);
  /* sweep 'survival' */
  sweepgen(L, g, psurvival, g->finobjold1, &dummy);
  g->finobjrold = g->finobjold1;
  g->finobjold1 = *psurvival;  /* 'survival' survivals are old now */
  g->finobjsur = g->finobj;  /* all news are survivals */

  // ͬ��������ɳ�tobefnz����
  sweepgen(L, g, &g->tobefnz, NULL, &dummy);
  // ��ÿ�ηִ�ʽ�㷨����ִ��ģʽִ�н��������
  finishgencycle(L, g);
}


/*
** Clears all gray lists, sweeps objects, and prepare sublists to enter
** generational mode. The sweeps remove dead objects and turn all
** surviving objects to old. Threads go back to 'grayagain'; everything
** else is turned black (not in any gray list).
*/
// �ִ�ʽ�㷨���л������ģʽ�Լ�ȫ��ִ��ģʽ�¶����õ�
static void atomic2gen (lua_State *L, global_State *g) {
  // cleargraylist�������ڳ�ʼ��һЩ״̬������һЩ����������ѻ�ɫ���������ñ����ָ�����á�
  // �������ѵ�ǰGC�׶�����Ϊ�����һ���׶�GCSswpallgc�׶Σ�������ǰ��ı�ǽ׶κ󣬿ɴ�����Ѿ��ɹ����Ϊ��ɫ�ˣ�
  // �����ʾҪ��ʼ��������׶��ˡ�
  cleargraylists(g);
  /* sweep all elements making them old */
  g->gcstate = GCSswpallgc;
  // ���ȵ�����sweep2old�������ú����������Ǳ��������е������������δ��ǵĶ����ѱ�ǵĶ���������ǵ�������ΪG_OLD�϶���
  sweep2old(L, &g->allgc);
  /* everything alive now is old */
  // ��reallyold��old1��survivalָ�붼ָ��allgc����ͷ��
  g->reallyold = g->old1 = g->survival = g->allgc;
  g->firstold1 = NULL;  /* there are no OLD1 objects anywhere */

  /* repeat for 'finobj' lists */
  // �����������ͬ����֮ͬ�������ﴦ��������д��������Ķ���Ҳ�Ƕ�����δ��ǵĶ��������������ѱ�ǵĶ����������������ΪG_OLD�϶���
  sweep2old(L, &g->finobj);
  g->finobjrold = g->finobjold1 = g->finobjsur = g->finobj;

  sweep2old(L, &g->tobefnz);

  // ��ʽ�޸�GC�㷨����gckindΪ�ִ�ʽ�㷨��Ȼ���ص�������ˣ��ѵ�ǰʵ���ڴ��������Ϊ�ڴ��׼ֵg->GCestimate��
  // GC����ʱ����ʹ�ø�ֵ�뵱ǰ�ڴ�ʹ�������бȽϣ���������ʹ�ò���ִ��ģʽ����ȫ��ִ��ģʽ��
  g->gckind = KGC_GEN;
  g->lastatomic = 0;
  g->GCestimate = gettotalbytes(g);  /* base for memory control */
  // ��ÿ�ηִ�ʽ�㷨����ִ��ģʽִ�н��������
  finishgencycle(L, g);
}


/*
** Set debt for the next minor collection, which will happen when
** memory grows 'genminormul'%.
*/
// ������һ����ǿ��GC��ʱ��
static void setminordebt (global_State *g) {
  luaE_setdebt(g, -(cast(l_mem, (gettotalbytes(g) / 100)) * g->genminormul));
}


/*
** Enter generational mode. Must go until the end of an atomic cycle
** to ensure that all objects are correctly marked and weak tables
** are cleared. Then, turn all objects into old and finishes the
** collection.
*/
// һ����ִ��һ������ʽ�������㷨��ִ����ɺ����úö�����������ڴ��׼ֵ
static lu_mem entergen (lua_State *L, global_State *g) {
  lu_mem numobjs;
  // �ڵ����л��ִ�ʽ�����Ĵ˿̣���ǰGC�㷨���ͱ�Ȼ��Ϊ��һ���㷨���ͼ�����ʽ�㷨��
  // luaC_runtilstate����������ʹGC�㷨���׶�˳�򡢲���ϵ�ִ��GC���̵�ĳ���׶�Ϊֹ���˴�����Ϊ�׶�ö��ֵGCSpause��
  // ����֪������GC���̵����һ���׶Σ�Ҳ�������Ϊ��һ�ֿ�ʼ�ĵ�һ���׶Ρ����������ʾ��������GC�㷨һֱ����ǰ�ֵ����׶�GCSpause�׶βŽ�����
  // 
  // �ִ�ʽ�㷨���ڸս��롢�Լ�ȫ��ִ��ģʽ�����󣬻�Ե�ǰ�ڴ�ʹ��������������ȷ��һ���ڴ��׼ֵ��
  // ���������ִ�ʽ�㷨ʱ�����������׼��������GCִ��ʹ�ò���ִ��ģʽ����ȫ��ִ��ģʽ��
  // ����Ϊ���ú������㷨ִ��ģʽѡ�����ȷ������ѡ��������ִ���걾������ʽ�㷨ʣ����������̣�
  // �����ڴ��е���������ȫ������ɾ����������Ļ�׼ֵ��������ȷ��
  luaC_runtilstate(L, bitmask(GCSpause));  /* prepare to start a new cycle */
  // ��ʱ�㷨������Ȼ����������ʽ�㷨���ͣ����¿�ʼ��һ�ֵ�GC�׶����̣���ִ�������߼���
  // ��ǽ׶α�ǳ�ʼ����㣬����ԭ�ӽ׶Σ��������н�����ɫ��ǣ������������ù�ϵ�������ϡ�
  luaC_runtilstate(L, bitmask(GCSpropagate));  /* start new cycle */
  numobjs = atomic(L);  /* propagates all and then do the atomic stuff */
  // ����ִ����Ϻ�����ͨ�����ù�ϵ�Ӹ����ɴ�Ķ��󶼽�˳�������Ϊ��ɫ�������ɴ�Ķ����򱣳ְ�ɫ��
  // ִ��ǰ��������ʱ���㷨��Ȼ����������ʽ�㷨ģʽ�£������ڸ�������ʽ�㷨�Ĵ������߼����̡�
  // Ȼ������һ����������׼��Ҫ���㷨�����л����ִ�ʽ�㷨
  atomic2gen(L, g);
  setminordebt(g);  /* set debt assuming next cycle will be minor */
  return numobjs;
}


/*
** Enter incremental mode. Turn all objects white, make all
** intermediate lists point to NULL (to avoid invalid pointers),
** and go to the pause state.
*/
// �������һЩ�ִ�ʽ�㷨ʹ�ù������ݽṹ��Ȼ���GC�㷨�����л�Ϊ����ʽ�������㷨
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
// GC�㷨�л�
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
// �ִ�GCȫ��ִ��ģʽ�������֮����ʵ����һ����ִ��һ������ʽ�������㷨��ִ����ɺ����úö�����������ڴ��׼ֵ
// ����һ������û�б�����ʱ�������ɲ�Ϊ��ɫ������ͬʱҲ��������һ������һ����������뵽����׶Ρ�
// ��͵�����������ɾ��һ����һ�����󣬸ö����������˰�ɫ������һ��������Щ����һ�������ܵõ���ȷ�ı����������
// ��������һ��������������Զ���ᱻ����ġ�
// ��͵��������ų�������У��ڲ���ִ��ģʽ�£��ڴ��в�������Ч��һ�������Խ��Խ�ࡣ����Ϊ�˱�����һ������ɾ�����⵼�µ��ڴ��������ǣ�
// �ִ�ʽ�㷨֧��������һ��ģʽ������ȫ��ִ��ģʽ��
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
// ����ȫ��ִ��ģʽ
// ȫ��ִ��ģʽ��������Ч�����������Ч�������󣬵���Ҫ����ȫ����������Ч�ʺܵ͡�
// ÿ��ִ��һ��ȫ��ִ��ģʽ�󣬷ִ�ʽ�㷨��Գɹ��ͷų������ڴ�������һ�������Ծ�����һ��GC����ʱ����Ϊ��
// 1�����ͷŵ��ڴ��������˻�׼����ֵ��һ�룬���ʾ����ȫ��ִ��ģʽ������ã���
// һ��GC����ʱ�������¸����ڴ�ʹ���������Ƿ�ʹ�ûز���ִ��ģʽ��
// 2�����ͷŵ��ڴ���������׼����ֵ��һ�룬���ʾ���ȫ��ִ��ģʽ���ϸ�����һ��GC������
// �ִ�ʽ�㷨�ͻ���õ���ȫ��ִ��ģʽ���ú�������fullgen�������кܶ�����߼���һ���ģ������һЩ�ж����߼�
static void stepgenfull (lua_State *L, global_State *g) {
  lu_mem newatomic;  /* count of traversed objects */
  lu_mem lastatomic = g->lastatomic;  /* count from last collection */
  // �ò����߼����ǰ��㷨�������л�������ʽ�㷨��Ȼ��ִ��ԭ�ӽ׶�����atomic���������ж�����б�ǣ�
  // atomic�ķ���ֵnewatomicΪԭ�ӽ׶��б���ǵĶ��������ע��˴������Ǳ�ǣ���δ��������׶����̡�
  if (g->gckind == KGC_GEN)  /* still in generational mode? */
    enterinc(g);  /* enter incremental mode */
  luaC_runtilstate(L, bitmask(GCSpropagate));  /* start new cycle */
  newatomic = atomic(L);  /* mark everybody */
  // lastatomic�Ƿִ�ʽ�㷨�����ڴ�����һ��GCԭ�ӽ׶��б���ǵĶ��������
  // �ô��ж�ʹ����λ��������3λ����ʮ���Ƴ���8�������Ըô��жϵ��߼��ǣ�
  // ����һ��GC��ǵĶ������û�б���һ��GC��ǵĶ��������8��֮1��
  // ��ζ������һ��ʹ��ȫ��ִ��ģʽ����ڴ�󣬱���GC����ʱ�ڴ�û�����Իص�������
  // �ֹ۵���Ϊ����ڴ�������Լ������ָý������������뻹�кü��ֲŻ��ٴ����´���ȫ��ִ��ģʽ��
  // ��ʱ���ִ�ʽ�㷨��ִ������Щ����ʽ�㷨�Ĳ������̺󣬼�������atomic2gen�����л��طִ�ʽ�㷨��
  // �������δ��Ƕ��������ڴ��׼ֵ��
  // ���һ��setminordebt�������������ڴ�ծ��ֵ����������GC����ʱ��
  if (newatomic < lastatomic + (lastatomic >> 3)) {  /* good collection? */
    atomic2gen(L, g);  /* return to generational mode */
    setminordebt(g);
  }
  else {  /* another bad collection; stay in incremental mode */
    // �����ֱ�ǵĶ�������������һ�ֶ���8��֮1����ִ�ʽ�㷨���۵���Ϊ����������£�
    // �´�GC����ʱ�����ܴ���ܻ��ٴ�ʹ��ȫ��ִ��ģʽ����ʱ�㷨���õķ����Ǻ���һֱ����ʹ����ȫ����ϵ�����ʽ�㷨��
    // �ö�����������ٵؽ��гɳ��������õĶ�������ٵؽ����������Ȼ�߼���Ҳ��ͬ��ʹ������ʽ�㷨��
    // ���������������һ�ж�g->lastatomic�ĸ�ֵ��������Ϊ0����ֵ��Ϊ0��ʱ����ÿ��GC�����ɽ��뵽�ִ�ʽ�㷨���߼��У�
    // Ȼ������GC���ջ��ǻ�����������stepgenfull�������ظ������̣�ֱ����һ�ֵ���ȫ����ʽ�㷨��ǵ�����С��8��֮1��
    // �Ż��÷ִ�ʽ�㷨�ָ��ز���ִ��ģʽ��
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
    // �̶�����֧��
    lu_mem majorbase = g->GCestimate;  /* memory after last major collection */
    // �������Ѽ��ֵ
    lu_mem majorinc = (majorbase / 100) * getgcparam(g->genmajormul);
    // ��ʵ���� > �̶����� + �������Ѽ��ֵ������ȫ��GC
    if (g->GCdebt > 0 && gettotalbytes(g) > majorbase + majorinc) {
      // ȫ��GC
      lu_mem numobjs = fullgen(L, g);  /* do a major collection */
      if (gettotalbytes(g) < majorbase + (majorinc / 2)) {
        /* collected at least half of memory growth since last major
           collection; keep doing minor collections. */
        // ��GC������ڴ治С�ڹ̶����ѵ�һ�룬����Ȼ������������1��Ԥ��ֵ��С��ֵ����Ȼʹ����ʵ���ѽ���5��֮1��ΪԤ��ֵ��
        // ����fullgen�Ѿ����ù���һ�������GC��ʱ�������ﲻ��Ҫ������
        lua_assert(g->lastatomic == 0);
      }
      else {  /* bad collection */
        // ��GC������ڴ�С�ڹ̶����ѵ�һ�룬˵������GCЧ�����ѣ����Ԥ��ֵ����Ľ�
        // ������GC����ص����������ʱ������ȥ��һЩ��Ч����������
        g->lastatomic = numobjs;  /* signal that last collection was bad */
        setpause(g);  /* do a long wait for next (major) collection */
      }
    }
    else {  /* regular case; do a minor collection */
      // �����GC
      youngcollection(L, g);
      // ������һ�������GC��ʱ��
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
// ��GC�׶�״ֵ̬��GCSenteratomic�л�����һ���׶�GCSwpallgc���⣬
// ��ͨ��sweeptolive�����ķ���ֵ����ʼ���������������g->sweepgc������ָ�룬
// ��ʾ����������g->sweepgcָ��ָ������λ�ÿ�ʼ
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


// ԭ�ӽ׶�
// ����׶��ܽ��������ʽ�㷨�������������⣺
// 1�������ɫ��������Table/UserData��ʹ�ú������ϱ�֤��ǽ׶�һ����ԭ��ʱ��������Ƶ���������ݸı䣬
// ������ֱ�Ӳ��뵽��Ǵ���gray��ɫ�����У���Ϊ������Ǳ�ڷ��գ�����Ǵ����׶����������������ܴ������������
// �ᵼ��gray������Զ�����꣬������Ҫ����һ������Ļ�ɫ����洢��Щʹ�ú������ϵĶ����Ҹ�������Ӱ���Ǵ����׶Ρ�
// 2���ȴ���ǽ�������Key_WeakTable�������ñ���Ҫһ���׶�ȥ�ȴ�������������ϲ�����������ֵ������̣�
// ��Ϊ�ڼ�δ����ǵ������Table�����ǲ�����������ֵ�ġ�
static lu_mem atomic (lua_State *L) {
  // �ڱ�ǹ��������⼸����Ǹ��������޸Ļ��滻������˰�ɫ�Ķ���Ҳ��û��Υ��һ����ԭ��
  // ��Ϊһ����ԭ��֤����û�к�ɫ�������ð�ɫ���󣬼�Ȼ��ˣ�LuaҲ�޷�ʹ������ȥ�޸����ָ��������޸���ɫ�쳣������
  // ������Ҫ����ɨ��
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
  // ֱ���Ȱ�grayagain����ֱ�Ӹ�ֵ��gray����Ȼ��������ֵ���gray����ִ�б�Ǵ�����
  // ��������ȷ��ʹ���˺������ϲ��뵽grayagain�������Щ�����ܵõ���ȷ�ı����
  g->gray = grayagain;
  work += propagateall(g);  /* traverse 'grayagain' list */
  // ���д���������ñ�
  convergeephemerons(g);
  // convergeephemerons�׶ν���������Table�ı�Ǿ�����ɣ�ͨ����˵��
  // ��ʱδ��ǵ�key��value���Ǻ�������׶���Ҫ�������������
  // ���Ƕ���Table������˵����������������и����ԣ�
  // ���ǵ�����Ԫ�أ���ֵ�ԣ�ĳ��value�����ʱ������keyҲ��Ҫ����գ��Ӷ����Դﵽɾ��ĳ��Ԫ�ص����á�
  // 
  // ֮����ֻ��Ҫ�������g->weak��g->allweak�е�key������ΪTableֻ����������ֵ�����ù�ϵ�У�
  // Table��value���п��ܳ���δ��Ƕ���Ӧ��key�����ѱ��״̬��Key_WeakTable���ڵ�g->ephemeron�����ǲ�������������ģ�
  // ����ֻ�����������������Ҫȥ����valueȥ���key��
  /* at this point, all strongly accessible objects are marked. */
  /* Clear values from weak tables, before checking finalizers */
  clearbyvalues(g, g->weak, NULL);
  clearbyvalues(g, g->allweak, NULL);
  // ԭ�ӽ׶���һ���־��ǰ���Ҫ�ͷŵĴ�FINALIZEDBIT��ǵĶ����g->finobj�������뵽g->tobefnz����
  // ����g->tobefnz�������ȫ�����б��
  origweak = g->weak; origall = g->allweak;
  separatetobefnz(g, 0);  /* separate objects to be finalized */
  work += markbeingfnz(g);  /* mark objects that will be finalized */
  work += propagateall(g);  /* remark, to propagate 'resurrection' */
  // ��Ϊ������g->tobefnz�����еĶ���ȫ������ˣ������������Ǵ����Ĺ����У����ܻ�������µ������ù�ϵ��
  // ��Щ��Ҳ��ͬ���ر������ͷ��뵽g->ephemeron��g->weak��g->allweak�����С�
  // ���ԣ�����������Ǵ����׶ν�����������Ҫ���´����ⲿ�ֵĴ��롣
  // ����Table��key����valueΪ�յļ�ֵ��Ԫ�ض����Ϊ��Ч�����״̬
  convergeephemerons(g);
  /* at this point, all resurrected objects are marked. */
  /* remove dead objects from weak tables */
  // �����ٴ�ʹ����clearbyvalues��������������֮ǰʹ�ò�ͬ������ε�3��������Ϊ���ˣ�
  // �ò����������Ҫ���ĸ�ָ��λ�ÿ�ʼ����������ͼ�д���origweak��origall��Ϊ��3��������
  // ��˼�Ǵӱ�����������̿�ʼͳ�ƣ�����֮������ӵ�Value_WeakTable��KeyValue_WeakTable������Ҫ���´����ǵĶ���
  // ��Ϊ����֮ǰ����ЩTable�Ѿ��Ǵ�����ˣ�����Ҫ�ٴδ���
  clearbykeys(g, g->ephemeron);  /* clear keys from all ephemeron tables */
  clearbykeys(g, g->allweak);  /* clear keys from all 'allweak' tables */
  /* clear values from resurrected weak tables */
  clearbyvalues(g, g->weak, origweak);
  clearbyvalues(g, g->allweak, origall);
  // ԭ�ӽ׶λ�����ַ�������g->strcache�������������������ַ������ʵ������ʵ�
  luaS_clearcache(g);
  // ��ԭ�ӽ׶ε���󣬻��current_white��other_white����
  g->currentwhite = cast_byte(otherwhite(g));  /* flip current white */
  lua_assert(g->gray == NULL);
  return work;  /* estimate of slots marked by 'atomic' */
}


// ����ʽ�㷨������׶��е�һ������������ú�����3����4�������ֱ��������֮��Ҫ
// �л�������һ���׶ε�ö��ֵ����һ���׶ν�Ҫ�����������
// 
// �ú���ͨ��if���������߼��ֳ����������
// 1) �ú����ڲ��ͻ�ʹ�õ�����������˵����Ҫ�ֶ�g->sweepgc�����ϸ��׶�����ָ��allgc�����е�Ԫ�ء�
// �ڱ������߼��ǣ���g->sweepgcָ��Ķ���Ϊ�գ������sweeplist����������g->sweepgcָ�������������д���
// ���ﺯ����3������countintΪGCSWEEPMAX����������ֵΪ100����������ʽ����׶�ÿ�����������������������Ϊ100��
// 2��g->sweepgcָ���������ÿ��sweepstep�����ɴ���100��������һ���ִι��������еĶ����ȫ��������ϣ�
// ��ʱg->sweepgcָ������ĩ�˵���һ��Ԫ�أ�����ָ�롣��ʱ���߼����ߵ���һ����֧��
// sweepstep���޸�GC�׶�Ϊ��һ���׶Σ�����g->sweepgc���������ָ��ָ����һ����Ҫ�����������
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


// GC�㷨���
static lu_mem singlestep (lua_State *L) {
  global_State *g = G(L);
  lu_mem work;
  lua_assert(!g->gcstopem);  /* collector is not reentrant */
  g->gcstopem = 1;  /* no emergency collections while collecting */
  switch (g->gcstate) {
    case GCSpause: {
      // �����ʼ�׶�
      restartcollection(g);
      g->gcstate = GCSpropagate;
      work = 1;
      break;
    }
    case GCSpropagate: {
      // ��Ǵ����׶�
      if (g->gray == NULL) {  /* no more gray objects? */
        g->gcstate = GCSenteratomic;  /* finish propagate phase */
        work = 0;
      }
      else
        work = propagatemark(g);  /* traverse one gray object */
      break;
    }
    case GCSenteratomic: {
      // ���ԭ�ӽ׶�
      work = atomic(L);  /* work is what was traversed by 'atomic' */
      // ɨ���ǽ�������������׶�
      entersweep(L);
      g->GCestimate = gettotalbytes(g);  /* first estimate */
      break;
    }
    case GCSswpallgc: {  /* sweep "regular" objects */
      // ��g->allgc�������ҵ���һ��δ��ǵĶ���Ҳ��������g->sweepgc��
      // �������Ϳ��������㷨����һ���֣�allgc��������׶Ρ����ӽ׶�ö��ֵΪ��GCSswpallgc����
      // ȫ�ơ�Garbage Collect State sweep allgc�������������������allgc�����׶�
      work = sweepstep(L, g, GCSswpfinobj, &g->finobj);
      break;
    }
    case GCSswpfinobj: {  /* sweep objects with finalizers */
      // 1�����д���δ��������������GCObject�����ڴ�����ʱ�򲢲��Ǵ洢��g->allgc�����еģ����ǻ�洢��g->finobj�����У�
      // 2����g->finobj�еĶ�����Ϊδ��ǽ�Ҫ���ͷ�ʱ������ԭ�ӽ׶ΰ����Ǵ�g->finobj�����Ƴ��������뵽g->tobefnz�����У�
      // ����ԭ�ӽ׶��ж�g->tobefnz��������е�Ԫ�ؽ��б�ǣ���ȷ��������GC����������ִ�н׶�֮ǰ��������δ��Ƕ�������׶α������
      // 3������������Ļ��ղ����2������GC�����У��ڵ�һ��GC������ִ��֮�󣬻�����Ǵ�g->tobefnz�����Ƴ�����ɾ������������ǣ�
      // �Ѷ�����һ�����������������²��뵽g->allgc�����С��ڵڶ�������GC�У���������Ȼû�б���ǣ�
      // ��ֻ��Ҫ����������ͨ�Ķ������������ɾ�����ڴ���ռ��ɡ�
      work = sweepstep(L, g, GCSswptobefnz, &g->tobefnz);
      break;
    }
    case GCSswptobefnz: {  /* sweep objects to be finalized */
      // ����׶ε����������sweepstep�������ڲ��ֵ�����sweeplist���ú����������δ��Ƕ������⣬���������øö������ɫ��ǡ�
      // g->tobefnz�����еĶ���ȷʵ��ԭ�ӽ׶α�����ˣ��������Ƕ��Ǻ�ɫ����������Ҫ��������ɫ����Ϊ��ǰ��ɫ��
      work = sweepstep(L, g, GCSswpend, NULL);
      break;
    }
    case GCSswpend: {  /* finish sweeps */
      // CSswpfinobj��GCSswptobefnz�׶ν����󣬶���������ص�g->finobj������g->tobefnz����������������ɫ���ò�����
      // �������ͽ��뵽������������׶�
      checkSizes(L, g);
      g->gcstate = GCScallfin;
      work = 0;
      break;
    }
    case GCScallfin: {  /* call remaining finalizers */
      // ��ԭ�ӽ׶��л�ѽ�Ҫ���ͷŵĴ��������Ķ�����뵽g->tobefnz�����У����Ը�g->tobefnz��������ж�����б���Է�ֹ����׶α������
      // ����GCScallfin��һ���׶Σ�����Ҫ��g->tobefnz����Ԫ�ؽ��д����ˣ��ý׶ε�����runafewfinalizers������ִ��һЩ��������
      // ���ô˺����Ĳ���GCFINMAXΪ����10������ÿ��GC���ִ��10������������
      // ÿһ��������������ִ�й�����������ΪGCFINALIZECOST�����ǳ���50����˼�Ƕ���GC���ԣ�ִ��һ�������������ܻ���Ԥ����
      // ��Ż������50������һ����
      if (g->tobefnz && !g->gcemergency) {
        g->gcstopem = 0;  /* ok collections during finalizers */
        work = runafewfinalizers(L, GCFINMAX) * GCFINALIZECOST;
      }
      else {  /* emergency mode or no more finalizers */
        // ��ȫ��������ִ����Ϻ�GC�ͻ����»ָ�����ʼ��GCSpause�׶Σ��ȴ���һ��GC�Ĵ���
        g->gcstate = GCSpause;  /* finish collection */
        work = 0;
      }
      break;
    }
    default: lua_assert(0); return 0;
  }
  g->gcstopem = 0;
  // ���������GC�����Ӧ�Ĺ�����
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
// ����ʽGC
// �㷨ÿ�δ������ڿ�ʼ�㷨������ʼ֮ǰ������ݵ�ǰǷ�µ�ծ���Լ���ҪԤ��ֵ�Ľ�
// ����һ����������������Ȼ��ͨ��whileѭ�����ϵ�ȥ������ֱ�������Щ���������������������GC
static void incstep (lua_State *L, global_State *g) {
  // �������� |1 ������Ϊ�˱��������0����
  int stepmul = (getgcparam(g->gcstepmul) | 1);  /* avoid division by 0 */
  // û������global_State�е�g->GCdebt����ʵ��ծ��������Ϊ�������g->GCdebt���жϣ�
  // ������GCû�ܹ��ɹ��ͷŵ��㹻�ڴ��ʱ�򣬱������㷨�ͻ������ѭ����
  // ����ִ���˹���Ĺ����Ž���������Ӱ������
  // ��˱������㷨û��ֱ��ʹ��g->GCdebt�������ڿ�ʼ��ʱ����������Զ���Ĺ�ʽת��Ϊ��������
  // Ȼ���ÿ��GC���̲�ֳ�һ���������Ĺ���singlestep��ÿ�������ۻ�һ�����������������������㹻�˾�ֹͣ����GC��
  // ������Ҫ�������GC�����е����̣����������ʽ�������㷨֮���Գ�֮Ϊ����ʽ���ɷֲ���������ȫ��ʽ�����ɴ�ϣ���ԭ��
  // 
  // ������ debt = (�ֿ���Ԥ��ֵ���ֵ��¶������Ķ��ڴ� / sizeof(TValue)) * 100
  l_mem debt = (g->GCdebt / WORK2MEM) * stepmul;
  // ��Ҫ��ϵͳԤ��ֵ�Ľ��
  // ֮���Ի���ծ���ҪԤ��ֵһ�ʽ�����Ϊ����������һ��ծ�񻺳壬�´�ծ������ʱ�����ȴ�Ԥ��ֵ�Ľ��۳���
  // ���Ա���ծ��Ѹ�ٴ���0���ٴο��ٴ���GC������Ч����GC��Ƶ��
  // ����˵�������㷨��ծ������㣺���Ǵ������Ĺ��̣�Ȼ���ڹ�������ɺ��ȫ�ֵĸ�ծg->GCdebt����Ϊ�Ǹ�Ԥ��ֵ�Ľ��
  // 
  // �����Ľ�������Ϊ��ϵͳԤ��ֵ�Ĺ���������ֵ����Ϊ��
  // stepsize = ��8KB / sizeof(TValue)) * 100
  // ������800KB��Ӧ��TValue�ĸ�����
  //
  // ��debt��stepsize��������ʽ��֪������豸���ܺã���Ҫÿ�ε����������չ��̣�singlestep�������������Ķ���
  // ��������ɸ���Ĺ��������Ե���stepmul����ֵ����ζ��ÿ�α������㷨��������Ҫ��ɸ���Ĺ�����Ԥ��ֵ����Ľ�
  // ����Ҫ��������TValue������������ܽ��������������չ��̡�
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
    // ��������㷨�෴����work units���ֽ���
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
      // �ִ�GC
      genstep(L, g);
    else
      // ����ʽGC
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


