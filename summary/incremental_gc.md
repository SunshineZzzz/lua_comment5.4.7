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
  ...
};
```

3. 三色标记清楚算法流程概述：
   
4. 三色标记清楚算法流程拆分：
  - GC开始阶段，GCSpause
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