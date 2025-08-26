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
  - 创建对象
  ```C
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

  // 分配一块大小为s的内存块空间
  void *luaM_malloc_ (lua_State *L, size_t size, int tag) {
    if (size == 0)
      return NULL;  /* that's all */
    else {
      global_State *g = G(L);
      void *newblock = firsttry(g, NULL, tag, size);
      if (l_unlikely(newblock == NULL)) {
        newblock = tryagain(L, NULL, tag, size);
        if (newblock == NULL)
          luaM_error(L);
      }
      // 增加债务
      g->GCdebt += size;
      return newblock;
    }
  }
  ```
  - 销毁对象
  ```C
  /*
  #define luaM_freemem(L, b, s)	luaM_free_(L, (b), (s))
  #define luaM_free(L, b)		luaM_free_(L, (b), sizeof(*(b)))
  #define luaM_freearray(L, b, n)   luaM_free_(L, (b), (n)*sizeof(*(b)))

  ** Free memory
  */
  // 释放内存
  void luaM_free_ (lua_State *L, void *block, size_t osize) {
    global_State *g = G(L);
    lua_assert((osize == 0) == (block == NULL));
    callfrealloc(g, block, osize, 0);
    // 减少债务
    g->GCdebt -= osize;
  }
  ```
  - 触发GC
  ```C
  /*
  ** Does one step of collection when debt becomes positive. 'pre'/'pos'
  ** allows some adjustments to be done only when needed. macro
  ** 'condchangemem' is used only for heavy tests (forcing a full
  ** GC cycle on every opportunity)
  */
  // 债务大于0就需要进行GC操作
  #define luaC_condGC(L,pre,pos) \
    { if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
      condchangemem(L,pre,pos); }

  /* more often than not, 'pre'/'pos' are empty */
  // 调用时机基本上是在创建GC对象的时候，具体可以查代码
  #define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)
  ```
  - 