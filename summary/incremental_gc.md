1. 开启三色标记清除算法
```lua
collectgarbage("incremental")
```
```c
/* kinds of Garbage Collection */
// 增量
#define KGC_INC		0	/* incremental gc */
// 分代
#define KGC_GEN		1	/* generational gc */

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
```

2. global_State中关于三色标记清除相关的数据
```C
typedef struct global_State {
  ...
  // 债务(需要回收的内存数量)，负数代表预充值多少金额到系统，正数代表需要偿还多少债务。
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
  // 所有GC对象创建之后都会放入该链表中
  GCObject *allgc;  /* list of all collectable objects */
  // 三色标记清除：回收链表，因为回收阶段可以分步进行，所以需要保存当前回收的位置,下一次从这个位置开始继续回收操作
  GCObject **sweepgc;  /* current position of sweep in list */
  // 三色标记清除：具有__gc元方法的GC对象链表
  GCObject *finobj;  /* list of collectable objects with finalizers */
  // 三色标记清楚：灰色链表
  GCObject *gray;  /* list of gray objects */
  // 具有open upvalue的状态机链表，只要当线程L第一次创建开放上值时，它需要被添加到G(L)->twups指向的链表中
  // G->twups => LN->twups => LN-1->twups => ... L1-> twups => NULL
  struct lua_State *twups;  /* list of threads with open upvalues */
  ...
};
```

3. 三色标记清楚算法流程概述：
   
4. 三色标记清楚算法流程拆分：
  - GC开始阶段，标记，GCSpause
  ```c
  case GCSpause: {
    // 开启新一轮增量GC，不可以分步进行
    // 标记
    restartcollection(g);
    // 切换到扫描阶段
    g->gcstate = GCSpropagate;
    work = 1;
    break;
  }

  /*
  ** mark root set and reset all gray lists, to start a new collection
  */
  // 标记
  static void restartcollection (global_State *g) {
    // 清空灰色链表和弱表相关
    cleargraylists(g);
    // 标记主状态机为灰色
    markobject(g, g->mainthread);
    // 标记全局注册表为灰色
    markvalue(g, &g->l_registry);
    // 标记基础类型对应的全局元表为灰色
    markmt(g);
    // 对上一轮g->tobefnz链表还未执行__gc元方法存留下的对象，标记为灰色
    markbeingfnz(g);  /* mark any finalizing object left from previous cycle */
  }
  ```
  - GC扫描阶段，GCSpropagate，这个阶段是可以分步进行
  ```C
  case GCSpropagate: {
    // 扫描
    if (g->gray == NULL) {  /* no more gray objects? */
      g->gcstate = GCSenteratomic;  /* finish propagate phase */
      work = 0;
    }
    else
      work = propagatemark(g);  /* traverse one gray object */
    break;
  }

  // 颜色扫描
  static lu_mem propagatemark (global_State *g) {
    GCObject *o = g->gray;
    // 自己标记为黑色
    nw2black(o);
    // 从gray脱离
    g->gray = *getgclist(o);  /* remove from 'gray' list */
    // 扫描所引用的内容
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
  ```
  ```C
  // 遍历table，标记所有可达的结点
  static lu_mem traversetable (global_State *g, Table *h) {
    const char *weakkey, *weakvalue;
    // 从元表中获取弱表信息
    const TValue *mode = gfasttm(g, h->metatable, TM_MODE);
    TString *smode;
    // 元表标记
    markobjectN(g, h->metatable);
    /*
    * local clearValue = {"123"}
    * local t = {
    *     ["value_key"] = clearValue
    * }
    * -- 声明值为弱引用
    * setmetatable(t, {__mode = "v"})
    * 
    * -- set value to nil
    * clearValue = nil
    * 
    * --  手动执行gc回收操作
    * collectgarbage()
    * 
    * print(t["value_key"])
    */
    // 1)"k": 声明键为弱引用，当键被置空时，table会清除这个结点
    // 2)"v": 声明值为弱引用，当值被置空时，table会清除这个结点
    // 3)"kv": 声明键和值都为弱引用，当其中一个被置空时，table会清除这个结点
    if (mode && ttisshrstring(mode) &&  /* is there a weak mode? */
        (cast_void(smode = tsvalue(mode)),
        cast_void(weakkey = strchr(getshrstr(smode), 'k')),
        cast_void(weakvalue = strchr(getshrstr(smode), 'v')),
        (weakkey || weakvalue))) {  /* is really weak? */
      if (!weakkey)  /* strong keys? */
        // 值为弱引用
        traverseweakvalue(g, h);
      else if (!weakvalue)  /* strong values? */
        // 键为弱引用
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

  // 遍历C闭包，标记所有可达的结点
  static int traverseCclosure (global_State *g, CClosure *cl) {
    int i;
    for (i = 0; i < cl->nupvalues; i++)  /* mark its upvalues */
      markvalue(g, &cl->upvalue[i]);
    return 1 + cl->nupvalues;
  }

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

  // 遍历线程（协程），标记所有可达的结点
  static int traversethread (global_State *g, lua_State *th) {
    UpVal *uv;
    StkId o = th->stack.p;
    if (isold(th) || g->gcstate == GCSpropagate)
      // 因为在GC的扫描阶段，可以分步，Lua代码可能仍然在运行并修改线程栈，所以GC无法保证一次扫描就能标记所有可达对象，
      // 将线程放入grayagain列表，意味着GC会在原子阶段（GCSatomic）再次安全地扫描它。
      linkgclist(th, g->grayagain);  /* insert into 'grayagain' list */
    if (o == NULL)
      return 1;  /* stack not completely built yet */
    lua_assert(g->gcstate == GCSatomic ||
              th->openupval == NULL || isintwups(th));
    // 标记栈上的值
    for (; o < th->top.p; o++)  /* mark live elements in the stack */
      markvalue(g, s2v(o));
    // 标记open upvalue
    for (uv = th->openupval; uv != NULL; uv = uv->u.open.next)
      markobject(g, uv);  /* open upvalues cannot be collected */
    if (g->gcstate == GCSatomic) {  /* final traversal? */
      if (!g->gcemergency)
        luaD_shrinkstack(th); /* do not change stack in emergency cycle */
      for (o = th->top.p; o < th->stack_last.p + EXTRA_STACK; o++)
        setnilvalue(s2v(o));  /* clear dead stack slice */
      /* 'remarkupvals' may have removed thread from 'twups' list */
      if (!isintwups(th) && th->openupval != NULL) {
        // 其他GC步骤可能会将线程从twups链表中移除，这里再加回去
        th->twups = g->twups;  /* link it back to the list */
        g->twups = th;
      }
    }
    return 1 + stacksize(th);
  }
  ```

  - 扫描阶段是可以分步进行，那就存在一个问题，黑色对象引用了新创建的白色对象，白色对象又没有被灰色对象引用，最终这个对象将会被错误清除，对于这种情况，lua采用2种方式应对：
    - 前向屏障，luaC_barrier_，直接将新创建的对象设置为灰色，并放入gray列表，其实这就是强三色不变式
    ```C
    // 前向屏障
    void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v) {
      global_State *g = G(L);
      lua_assert(isblack(o) && iswhite(v) && !isdead(g, v) && !isdead(g, o));
      // 是否需要保持​​强三色不变性(黑色对象绝对不能直接引用任何白色对象)
      if (keepinvariant(g)) {  /* must keep invariant? */
        // 进行染色
        reallymarkobject(g, v);  /* restore invariant */
        if (isold(o)) {
          lua_assert(!isold(v));  /* white object could not be old */
          setage(v, G_OLD0);  /* restore generational invariant */
        }
      }
      else {  /* sweep phase */
        lua_assert(issweepphase(g));
        if (g->gckind == KGC_INC)  /* incremental mode? */
          // 清理阶段不需要保持，直接置白色(白色切换了)，不会清楚
          makewhite(g, o);  /* mark 'o' as white to avoid other barriers */
      }
    }
    ```

    - 后向屏障，luaC_barrierback_，将黑色对象设置为灰色，然后放入grayagain列表，这个应该就是弱三色不变式
    ```C
    // 后向屏障
    void luaC_barrierback_ (lua_State *L, GCObject *o) {
      global_State *g = G(L);
      lua_assert(isblack(o) && !isdead(g, o));
      lua_assert((g->gckind == KGC_GEN) == (isold(o) && getage(o) != G_TOUCHED1));
      if (getage(o) == G_TOUCHED2)  /* already in gray list? */
        set2gray(o);  /* make it gray to become touched1 */
      else  /* link it in 'grayagain' and paint it gray */
        // 新创建白色对象被引用的黑色对象直接放入grayagain链表，应该是弱三色不变式
        // 如果放入gray链表，扫描阶段有可能没法结束，或者延迟结束
        linkobjgclist(o, g->grayagain);
      if (isold(o))  /* generational mode? */
        setage(o, G_TOUCHED1);  /* touched in current cycle */
    }    
    ```
  
  - 原子阶段，为什么需要原子阶段(https://manistein.github.io/blog/post/program/let-us-build-a-lua-interpreter/%E6%9E%84%E5%BB%BAlua%E8%A7%A3%E9%87%8A%E5%99%A8part2/)？
  
  >为什么要原子执行atomic阶段？如果atomic阶段不能原子执行，那么就和propagate阶段没有区别了，table从黑色被标记为灰色，放入grayagain列表也失去了意义，因为如果不能原子执行，那么该table对象很可能前脚刚在grayagain列表中pop出来，由灰色变黑色，后脚又有新的对象被它引用，又将它从黑色变为灰色，因此避免不了一些table实例，在黑色和灰色之间来回切换，反复标记和扫描，浪费性能，很可能导致gc在标记和扫描阶段所处的时间过长，甚至无法进入sweep阶段，因此干脆在atomic阶段设置为不可中断执行，一次完成所有的标记和扫描操作。这样，一个能够被频繁改变引用关系的table对象，最多在progapate阶段的时候被标记和扫描一次，在atomic阶段又被扫描一次，一共两次。
  
  ```C
  case GCSenteratomic: {
    // 原子阶段
    work = atomic(L);  /* work is what was traversed by 'atomic' */
    // 进入清除阶段
    entersweep(L);
    g->GCestimate = gettotalbytes(g);  /* first estimate */
    break;
  }
  ```