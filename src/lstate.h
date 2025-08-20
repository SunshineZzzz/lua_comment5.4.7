/*
** $Id: lstate.h $
** Global State
** See Copyright Notice in lua.h
*/

#ifndef lstate_h
#define lstate_h

#include "lua.h"


/* Some header files included here need this definition */
typedef struct CallInfo CallInfo;


#include "lobject.h"
#include "ltm.h"
#include "lzio.h"


/*
** Some notes about garbage-collected objects: All objects in Lua must
** be kept somehow accessible until being freed, so all objects always
** belong to one (and only one) of these lists, using field 'next' of
** the 'CommonHeader' for the link:
**
** 'allgc': all objects not marked for finalization;
** 'finobj': all objects marked for finalization;
** 'tobefnz': all objects ready to be finalized;
** 'fixedgc': all objects that are not to be collected (currently
** only small strings, such as reserved words).
**
** For the generational collector, some of these lists have marks for
** generations. Each mark points to the first element in the list for
** that particular generation; that generation goes until the next mark.
**
** 'allgc' -> 'survival': new objects;
** 'survival' -> 'old': objects that survived one collection;
** 'old1' -> 'reallyold': objects that became old in last collection;
** 'reallyold' -> NULL: objects old for more than one cycle.
**
** 'finobj' -> 'finobjsur': new objects marked for finalization;
** 'finobjsur' -> 'finobjold1': survived   """";
** 'finobjold1' -> 'finobjrold': just old  """";
** 'finobjrold' -> NULL: really old       """".
**
** All lists can contain elements older than their main ages, due
** to 'luaC_checkfinalizer' and 'udata2finalize', which move
** objects between the normal lists and the "marked for finalization"
** lists. Moreover, barriers can age young objects in young lists as
** OLD0, which then become OLD1. However, a list never contains
** elements younger than their main ages.
**
** The generational collector also uses a pointer 'firstold1', which
** points to the first OLD1 object in the list. It is used to optimize
** 'markold'. (Potentially OLD1 objects can be anywhere between 'allgc'
** and 'reallyold', but often the list has no OLD1 objects or they are
** after 'old1'.) Note the difference between it and 'old1':
** 'firstold1': no OLD1 objects before this point; there can be all
**   ages after it.
** 'old1': no objects younger than OLD1 after this point.
*/

/*
** Moreover, there is another set of lists that control gray objects.
** These lists are linked by fields 'gclist'. (All objects that
** can become gray have such a field. The field is not the same
** in all objects, but it always has this name.)  Any gray object
** must belong to one of these lists, and all objects in these lists
** must be gray (with two exceptions explained below):
**
** 'gray': regular gray objects, still waiting to be visited.
** 'grayagain': objects that must be revisited at the atomic phase.
**   That includes
**   - black objects got in a write barrier;
**   - all kinds of weak tables during propagation phase;
**   - all threads.
** 'weak': tables with weak values to be cleared;
** 'ephemeron': ephemeron tables with white->white entries;
** 'allweak': tables with weak keys and/or weak values to be cleared.
**
** The exceptions to that "gray rule" are:
** - TOUCHED2 objects in generational mode stay in a gray list (because
** they must be visited again at the end of the cycle), but they are
** marked black because assignments to them must activate barriers (to
** move them back to TOUCHED1).
** - Open upvales are kept gray to avoid barriers, but they stay out
** of gray lists. (They don't even have a 'gclist' field.)
*/



/*
** About 'nCcalls':  This count has two parts: the lower 16 bits counts
** the number of recursive invocations in the C stack; the higher
** 16 bits counts the number of non-yieldable calls in the stack.
** (They are together so that we can change and save both with one
** instruction.)
*/


/* true if this thread does not have non-yieldable calls in the stack */
// nCcalls高16位判断是否可以yield
#define yieldable(L)		(((L)->nCcalls & 0xffff0000) == 0)

/* real number of C calls */
// nCcalls低16位来作为c函数嵌套深度
#define getCcalls(L)	((L)->nCcalls & 0xffff)


/* Increment the number of non-yieldable calls */
// 表示线程不可以挂起
#define incnny(L)	((L)->nCcalls += 0x10000)

/* Decrement the number of non-yieldable calls */
// 减去上面增加的值
#define decnny(L)	((L)->nCcalls -= 0x10000)

/* Non-yieldable call increment */
// 
#define nyci	(0x10000 | 1)




struct lua_longjmp;  /* defined in ldo.c */


/*
** Atomic type (relative to signals) to better ensure that 'lua_sethook'
** is thread safe
*/
#if !defined(l_signalT)
#include <signal.h>
#define l_signalT	sig_atomic_t
#endif


/*
** Extra stack space to handle TM calls and some other extras. This
** space is not included in 'stack_last'. It is used only to avoid stack
** checks, either because the element will be promptly popped or because
** there will be a stack check soon after the push. Function frames
** never use this extra space, so it does not need to be kept clean.
*/
#define EXTRA_STACK   5


#define BASIC_STACK_SIZE        (2*LUA_MINSTACK)

#define stacksize(th)	cast_int((th)->stack_last.p - (th)->stack.p)


/* kinds of Garbage Collection */
// 增量
#define KGC_INC		0	/* incremental gc */
// 分代
#define KGC_GEN		1	/* generational gc */

// 短字符串全局表
typedef struct stringtable {
  // hashtable，散列桶存放数据
  /*
  * [TString*] -> [TString] -> [TString] -> ... -> [TString]
  * [Tstring*] -> [TString] -> [TString] -> ... -> [TString]
  * ...
  * [Tstring*] -> [TString] -> [TString] -> ... -> [TString]
  */
  TString **hash;
  // nuse表示当前有多少个经过内部化处理的短字符串
  int nuse;  /* number of elements */
  // size表示stringtable结构内部的hash数组的⼤⼩
  int size;
} stringtable;


/*
** Information about a call.
** About union 'u':
** - field 'l' is used only for Lua functions;
** - field 'c' is used only for C functions.
** About union 'u2':
** - field 'funcidx' is used only by C functions while doing a
** protected call;
** - field 'nyield' is used only while a function is "doing" an
** yield (from the yield until the next resume);
** - field 'nres' is used only while closing tbc variables when
** returning from a function;
** - field 'transferinfo' is used only during call/returnhooks,
** before the function starts or after it ends.
*/
// 调用信息
// Lua闭包与Lua运行栈的中间桥梁
struct CallInfo {
  // 函数地址指针，指向CallInfo对应的Lua闭包在运行栈中的位置。CallInfo初始化的时候赋值，运行时不变
  StkIdRel func;  /* function index in the stack */
  // 函数在栈中的顶部指针，函数在执行的时候有可能会需要继续往运行栈中插入数据，
  // 插入数据的时候Lua运行栈的top指针会往上（顶部）移动，
  // 而CallInfo的top指针指向插入数据后会到达的最高的Lua运行栈的位置(运行后lua_State的top最大值)
  // CallInfo初始化的时候赋值，运行时不变。
  StkIdRel	top;  /* top for this function */
  // 前向、后向指针，在同一个Lua线程之中，函数的调用是单向串连的，不可能在同一时刻同时执行着两段函数
  // 不过在函数内部是可以调用其它函数的，previous和next指针，就是用于把当前未结束的所有函数串连起来
  // 定义了3个函数f1、f2、f3，其中f1函数内部调用了f2，f2内部调用了f3
  // null<-*previous-f1<=>f2<=>f3-*next->null
  struct CallInfo *previous, *next;  /* dynamic call link */
  union {
    // 存储Lua闭包运行时的信息
    struct {  /* only for Lua functions */
      // 用于记录当前虚拟机执行器执行到当前函数的哪条指令
      const Instruction *savedpc;
      // 当前调用是否需要被捕捉，可视作布尔变量
      // 用于表示hook函数钩子功能是否开启，开启后会根据lua_State的hookmask字段决定要钩起监听的逻辑，
      // 当trap为1时即代表该次调用hook功能开启了，告诉执行器需要在当前CallInfo指令执行的过程中，
      // 跳转并执行特定的自定义钩子函数
      volatile l_signalT trap;  /* function is tracing lines/counts */
      // next extra args，表示额外传入的函数参数的个数
      // 用于运行过程中对堆栈进行修正，比如add(a,b)函数，需要两个参数，
      // 但我们调用的时候写成了add(1,2,3)，传入了3个参数，多了一个参数，
      // 所以此时额外传入参数nextraargs即为1
      // 通常与上面提到的nresult字段配合进行返回值数量调节
      int nextraargs;  /* # of extra arguments in vararg functions */
    } l;
    struct {  /* only for C functions */
      lua_KFunction k;  /* continuation in case of yields */
      ptrdiff_t old_errfunc;
      lua_KContext ctx;  /* context info. in case of yields */
    } c;
  } u;
  union {
    int funcidx;  /* called-function index */
    // yield要返回的参数个数
    int nyield;  /* number of values yielded */
    int nres;  /* number of values returned */
    struct {  /* info about transferred values (for call/return hooks) */
      unsigned short ftransfer;  /* offset of first value transferred */
      unsigned short ntransfer;  /* number of values transferred */
    } transferinfo;
  } u2;
  // 调用该函数期待的返回值个数
  // CallInfo初始化的时候赋值，由上层调用者决定，运行时不变
  // 用于函数调用完毕后调节返回值数量
  short nresults;  /* expected number of results from this function */
  // 代表当前函数的调用状态
  // 使用位标记同时表达多种可并存的状态，该字段运行时可动态改变
  // 其中这些状态的CIST前缀全称为“CallInfo Status”
  unsigned short callstatus;
};


/*
** Bits in CallInfo status
*/
#define CIST_OAH	(1<<0)	/* original value of 'allowhook' */
// CallInfo调用的是c函数
#define CIST_C		(1<<1)	/* call is running a C function */
// 尚未开始执行的调用
#define CIST_FRESH	(1<<2)	/* call is on a fresh "luaV_execute" frame */
#define CIST_HOOKED	(1<<3)	/* call is running a debug hook */
#define CIST_YPCALL	(1<<4)	/* doing a yieldable protected call */
#define CIST_TAIL	(1<<5)	/* call was tail called */
#define CIST_HOOKYIELD	(1<<6)	/* last hook called yielded */
#define CIST_FIN	(1<<7)	/* function "called" a finalizer */
#define CIST_TRAN	(1<<8)	/* 'ci' has transfer information */
#define CIST_CLSRET	(1<<9)  /* function is closing tbc variables */
/* Bits 10-12 are used for CIST_RECST (see below) */
#define CIST_RECST	10
#if defined(LUA_COMPAT_LT_LE)
#define CIST_LEQ	(1<<13)  /* using __lt for __le */
#endif


/*
** Field CIST_RECST stores the "recover status", used to keep the error
** status while closing to-be-closed variables in coroutines, so that
** Lua can correctly resume after an yield from a __close method called
** because of an error.  (Three bits are enough for error status.)
*/
#define getcistrecst(ci)     (((ci)->callstatus >> CIST_RECST) & 7)
#define setcistrecst(ci,st)  \
  check_exp(((st) & 7) == (st),   /* status must fit in three bits */  \
            ((ci)->callstatus = ((ci)->callstatus & ~(7 << CIST_RECST))  \
                                                  | ((st) << CIST_RECST)))


/* active function is a Lua function */
#define isLua(ci)	(!((ci)->callstatus & CIST_C))

/* call is running Lua code (not a hook) */
#define isLuacode(ci)	(!((ci)->callstatus & (CIST_C | CIST_HOOKED)))

/* assume that CIST_OAH has offset 0 and that 'v' is strictly 0/1 */
#define setoah(st,v)	((st) = ((st) & ~CIST_OAH) | (v))
#define getoah(st)	((st) & CIST_OAH)


/*
** 'global state', shared by all threads of this state
*/
// 全局状态机
typedef struct global_State {
  lua_Alloc frealloc;  /* function to reallocate memory */
  void *ud;         /* auxiliary data to 'frealloc' */
  // 在程序运行中，随着内存的不断使用，真实消费会不断增加，因为增长的消费金额先消耗预充值部分，
  // 当消费增加的部分超过了预充值的金额，说明预充值的金额耗尽了，又要去借债了，此时则会触发GC。
  // GC触发后会继续处理那些未被处理完毕的消费订单，GC结束后则会再次预充值一笔金额并等待下次GC的重新开始
  // 个人总消费，数值等于真实消费（allocated已分配的总内存）+ 预充值到系统的金额（用于后续抵消债务）。
  l_mem totalbytes;  /* number of bytes currently allocated - GCdebt */
  // 债务，负数的话其绝对值代表预充值多少金额到系统；正数代表需要偿还多少债务。
  l_mem GCdebt;  /* bytes allocated not yet compensated by the collector */
  // 非垃圾对象内存占用。在GC中已经处理完毕，被视为合法非垃圾对象的内存占用，
  // 在债务算法中可理解为消费后已经确认的订单消费金额
  // 在分代GC中，可以理解为固定消费支出
  lu_mem GCestimate;  /* an estimate of the non-garbage memory in use */
  // 分代式算法中用于代表上一轮GC原子阶段中被标记的对象个数
  // 然后下轮GC最终还是会回来这个stepgenfull函数，重复该流程，直到新一轮的完全增量式算法标记的数量小于8分之1，
  // 才会让分代式算法恢复回部分执行模式。
  lu_mem lastatomic;  /* see function 'genstep' in file 'lgc.c' */
  // 字符串hashtable，算是二级缓存
  stringtable strt;  /* hash table for strings */
  // 全局注册表
  // LUA_RIDX_MAINTHREAD(1-1=0)：指向 主线程
  // LUA_RIDX_GLOBALS(2-1=1)：存储全局变量
  // LUA_LOADED_TABLE("_LOADED")：存储模块（包括系统默认加载的模块，以及用户 require 的模块）
  // full user data 的 tname <-> 原表{__name=tname}
  TValue l_registry;
  // 空值
  TValue nilvalue;  /* a nil value */
  unsigned int seed;  /* randomized seed for hashes */
  lu_byte currentwhite;
  lu_byte gcstate;  /* state of garbage collector */
  // GC算法类型，增量式/分代
  lu_byte gckind;  /* kind of GC running */
  lu_byte gcstopem;  /* stops emergency collections */
  // 年轻代GC参数，默认值20
  // GC完成后会把真实消费金额的100分之20，即5分1作为预充值金额
  lu_byte genminormul;  /* control for minor generational collections */
  // 全代GC参数，默认值100
  // 此数值开发者也可以通过函数在程序运行时进行修改，若设备内存充足，可调大数值，
  // 意味着支持更大内存增长的时候再采用全员征税的方案，否则都只向年轻一代征税
  // 如果保持100这个默认值，通过公式计算相当于新增消费检测值与固定消费相等
  // 此时当真实消费大于两者之和时，则触发全员征税
  // 
  // 默认值为100，所以可以理解为若内存没有增长到达（100%）基准值的2倍，则保持部分执行模式，否则全部模式
  lu_byte genmajormul;  /* control for major generational collections */
  lu_byte gcstp;  /* control whether GC is running */
  lu_byte gcemergency;  /* true if this is an emergency collection */
  lu_byte gcpause;  /* size of pause between successive GCs */
  // 步进倍率
  lu_byte gcstepmul;  /* GC "speed" */
  // 在下一个GC步骤之前这次GC回收内存对应的Tvalue量
  lu_byte gcstepsize;  /* (log2 of) GC granularity */
  // GCObject对象指针链表
  // [allgc, survival)区间对象：是在上轮GC结束后，本轮GC开始前，新创建的对象，属于年轻一代对象中最年轻的一批，年龄对应为G_NEW，
  // 若在本轮GC中未被标记则会被清除。
  GCObject *allgc;  /* list of all collectable objects */
  // 指向GCObject*的指针，指向当前正在被清除的GCObject链表中的某个元素，它就像是容器遍历的迭代器，
  // 记录了当前遍历到的是哪个容器的哪个元素，通过该迭代器元素的next字段，可以让迭代器继续往后遍历容器的下一个元素
  GCObject **sweepgc;  /* current position of sweep in list */
  // 当对象拥有此标记FINALIZEDBIT时，代码它拥有未执行的析构器，
  // 而这些对象它们在创建时也将会从g->allgc通用GCObject链表移除，
  // 并放在g->finobj链表中进行管理。
  // [allgc, finobjsur)区间对象：G_NEW
  GCObject *finobj;  /* list of collectable objects with finalizers */
  // 通常染色函数会把对象设置为黑色，但若该对象还引用了其它GCObject对象，
  // 则先设置当前对象为灰色，并把该对象链接到global_State的gray链表中，
  // 后续当开始处理它引用的子对象的时候再把它自身设置为黑色
  GCObject *gray;  /* list of gray objects */
  // 在标记过程中，当有Table/UserData发生对象改变，会使用后退屏障保证一致性原则
  // 为了避免gray链表因为每轮新增对象多于每轮能处理的对象，导致永远处理不完，
  // 处理方式是不把使用后退屏障的对象插入到gray链表，而是把对象插入到grayagain灰色链表中
  GCObject *grayagain;  /* list of objects to be traversed atomically */
  // Value_WeakTable会存储于g->weak链表中
  GCObject *weak;  /* list of tables with weak values */
  // 在原子阶段中Key_WeakTable会存储在g->ephemeron链表
  GCObject *ephemeron;  /* list of ephemeron tables (weak keys) */
  // KeyValue_WeakTable会存储于g->allweak链表中
  GCObject *allweak;  /* list of all-weak tables */
  // 在本轮GC中，将要被清除的带__gc元方法析构器的对象，会由finobj链表移出，并移入到tobefnz链表中进行存储管理，
  // 在清除阶段结束后tobefnz链表中对象的析构器方法会被统一执行；
  GCObject *tobefnz;  /* list of userdata to be GC */
  // 避免被GC
  GCObject *fixedgc;  /* list of objects not to be collected */
  /* fields for generational collector */
  // [survival, old1)区间对象：该区间对象由上述区间的G_NEW对象成长而来，也属于年轻一代对象，年龄对应为G_SURVIVAL，
  // 若在本轮GC中未被标记则也会被清除。
  GCObject *survival;  /* start of objects that survived one GC cycle */
  // [old1, reallyold)区间对象：由上述区间成长而来，年龄对应为G_OLD1，他们属于老一代对象。但它们是刚刚成为老对象，
  // 刚刚获得资格参与部分老对象独有的标记流程。这些对象在本轮中还可能引用着待标记的白色年轻一代对象，所以G_OLD1对象他们要参与本轮的GC标记传播逻辑，
  // 把黑色标记传播给引用的这些年轻一代子结点；
  // 但毕竟作为老对象，他们都是GC根结点引用可达的，他们已经不再需要参与GC清除流程了，即在分代式算法部分执行模式下，他们已经不再会被清除了。
  GCObject *old1;  /* start of old1 objects */
  // [reallyold, 链表末尾] 区间对象：最老一代对象，年龄对应为G_OLD它们是以老对象身份存活了超过1轮GC的对象，
  // 他们自身已经不再有引用的任何白色年轻对象，这些元老级对象已经不再需要参与任何GC流程了，也不会再被清除了。
  GCObject *reallyold;  /* objects more than one cycle old ("really old") */
  // firstold1指针，则是在上一轮GC处理完成后缓存的指向allgc链表中的第一个G_OLD1年龄对象的指针
  GCObject *firstold1;  /* first OLD1 object in the list (if any) */
  // 与上述allgc链表新增指针的逻辑几乎一样，finobj链表也相对应的根据年龄新增了3个指针，都以finobj作为前缀，
  // 不同的地方在于finobj链表中的对象都是声明了__gc元方法的带有析构器的对象：
  // [finobjsur, old1)区间对象：G_SURVIVAL
  GCObject *finobjsur;  /* list of survival objects with finalizers */
  // [finobjold1, reallyold)区间对象：G_OLD1
  GCObject *finobjold1;  /* list of old1 objects with finalizers */
  // [finobjrold, 链表末尾]区间对象：G_OLD
  GCObject *finobjrold;  /* list of really old objects with finalizers */
  // 拥有UpValue的协程链表，一个对象如果是被协程通过UpValue引用了，
  // 尽管该对象可能已经脱离了它原本声明的作用域（例如局部函数已经结束），
  // 但在协程生命周期结束之前也是不能被回收，所以标记阶段需要对这些协程以及他们引用了的UpValue进行标记。
  struct lua_State *twups;  /* list of threads with open upvalues */
  lua_CFunction panic;  /* to be called in unprotected errors */
  // 当前运行的状态机
  struct lua_State *mainthread;
  // 初始为"not enough memory"该字符串永远不会被回收
  TString *memerrmsg;  /* message for memory-allocation errors */
  // 初始化为元方法字符串, 且将它们标记为不可回收对象
  TString *tmname[TM_N];  /* array with tag-method names */
  // 基础类型的元表
  struct Table *mt[LUA_NUMTYPES];  /* metatables for basic types */
  // 字符串缓存，算是1级缓存
  TString *strcache[STRCACHE_N][STRCACHE_M];  /* cache for strings in API */
  // 警告函数
  lua_WarnFunction warnf;  /* warning function */
  // 警告函数的参数
  void *ud_warn;         /* auxiliary data to 'warnf' */
} global_State;


/*
** 'per thread' state
*/
// lua状态机
struct lua_State {
  CommonHeader;
  // 当前状态机的状态
  lu_byte status;
  // 当前状态机是否允许hook
  lu_byte allowhook;
  // 链表中所有函数的数量
  unsigned short nci;  /* number of items in 'ci' list */
  // 指向栈的顶部下一个，压入数据，都通过移动这个指针来实现
  StkIdRel top;  /* first free slot in the stack */
  // 这里就是指向一个全局状态结构体，所有lua_State指向同一个global_State，
  // 所以Lua中Thread可以共享到这些全局数据
  global_State *l_G;
  // 指向当前执行函数（链表）的指针
  CallInfo *ci;  /* call info for current function */
  // Lua执行代码是通过堆栈的，这里就是记录堆栈的头指针
  StkIdRel stack_last;  /* end of stack (last element + 1) */
  // Lua执行代码是通过堆栈的，这里就是记录堆栈尾部指针
  StkIdRel stack;  /* stack base */
  // 可以理解为所有（open状态）的UpVal都链接在这个链表当中，
  // 按照各个UpVal变量的声明顺序，后声明的会链接在表头，先声明的会保留在链表末端，
  // 然后根据他们在链表的深度，会依次给他们一个level值
  UpVal *openupval;  /* list of open upvalues in this stack */
  // 函数运行完成后准备关闭的函数
  // 记录着最后一个tbc节点，栈缩容时会判断该节点是否在缩容空间内，
  // 如果在，那么就根据这个节点调用缩容空间内所有tbc变量的__close()元方法；
  // 需要注意的是，这里节点的链接不是通过指针，而是通过相邻tbc变量在栈中的距离
  StkIdRel tbclist;  /* list of to-be-closed variables */
  // 用于GC垃圾回收算法链接到某个回收队列
  GCObject *gclist;
  // 
  struct lua_State *twups;  /* list of threads with open upvalues */
  // 常用于保护模式下运行某个函数，若发生错误的时候会调用跳转指令跳转到这个位置
  struct lua_longjmp *errorJmp;  /* current error recover point */
  // 当前状态机运行时候的入口函数
  CallInfo base_ci;  /* CallInfo for first level (C calling Lua) */
  // 函数勾子
  volatile lua_Hook hook;
  // 若有设置此错误回调函数，则运行发生错误后会调用这个函数，通常用于输出异常信息。
  ptrdiff_t errfunc;  /* current error handling function (stack index) */
  // 当前执行的函数的深度
  l_uint32 nCcalls;  /* number of nested (non-yieldable | C)  calls */
  int oldpc;  /* last pc traced */
  // 用户设置的执行指令数（在hookmask=LUA_MASK_COUNT生效）
  int basehookcount;
  // 运行时，跑了多少条指令
  int hookcount;
  volatile l_signalT hookmask;
};


#define G(L)	(L->l_G)

/*
** 'g->nilvalue' being a nil value flags that the state was completely
** build.
*/
#define completestate(g)	ttisnil(&g->nilvalue)


/*
** Union of all collectable objects (only for conversions)
** ISO C99, 6.5.2.3 p.5:
** "if a union contains several structures that share a common initial
** sequence [...], and if the union object currently contains one
** of these structures, it is permitted to inspect the common initial
** part of any of them anywhere that a declaration of the complete type
** of the union is visible."
*/
// 模拟父类向子类转换的中间态，主要是目的就是转换
union GCUnion {
  GCObject gc;  /* common header */
  // lua string
  struct TString ts;
  // full user data
  struct Udata u;
  union Closure cl;
  // lua table
  struct Table h;
  struct Proto p;
  struct lua_State th;  /* thread */
  struct UpVal upv;
};


/*
** ISO C99, 6.7.2.1 p.14:
** "A pointer to a union object, suitably converted, points to each of
** its members [...], and vice versa."
*/
// GCObject 转换成中间态 GCUnion
#define cast_u(o)	cast(union GCUnion *, (o))

/* macros to convert a GCObject into a specific value */
#define gco2ts(o)  \
	check_exp(novariant((o)->tt) == LUA_TSTRING, &((cast_u(o))->ts))
// GCObject 转换成 user full data
#define gco2u(o)  check_exp((o)->tt == LUA_VUSERDATA, &((cast_u(o))->u))
#define gco2lcl(o)  check_exp((o)->tt == LUA_VLCL, &((cast_u(o))->cl.l))
#define gco2ccl(o)  check_exp((o)->tt == LUA_VCCL, &((cast_u(o))->cl.c))
#define gco2cl(o)  \
	check_exp(novariant((o)->tt) == LUA_TFUNCTION, &((cast_u(o))->cl))
// GCObject 转换成 Table
#define gco2t(o)  check_exp((o)->tt == LUA_VTABLE, &((cast_u(o))->h))
#define gco2p(o)  check_exp((o)->tt == LUA_VPROTO, &((cast_u(o))->p))
#define gco2th(o)  check_exp((o)->tt == LUA_VTHREAD, &((cast_u(o))->th))
// GCObject 转换成 upv
#define gco2upv(o)	check_exp((o)->tt == LUA_VUPVAL, &((cast_u(o))->upv))


/*
** macro to convert a Lua object into a GCObject
** (The access to 'tt' tries to ensure that 'v' is actually a Lua object.)
*/
// GCObject从中取出GC部分
#define obj2gco(v)	check_exp((v)->tt >= LUA_TSTRING, &(cast_u(v)->gc))


/* actual number of total bytes allocated */
#define gettotalbytes(g)	cast(lu_mem, (g)->totalbytes + (g)->GCdebt)

LUAI_FUNC void luaE_setdebt (global_State *g, l_mem debt);
LUAI_FUNC void luaE_freethread (lua_State *L, lua_State *L1);
LUAI_FUNC CallInfo *luaE_extendCI (lua_State *L);
LUAI_FUNC void luaE_shrinkCI (lua_State *L);
LUAI_FUNC void luaE_checkcstack (lua_State *L);
LUAI_FUNC void luaE_incCstack (lua_State *L);
LUAI_FUNC void luaE_warning (lua_State *L, const char *msg, int tocont);
LUAI_FUNC void luaE_warnerror (lua_State *L, const char *where);
LUAI_FUNC int luaE_resetthread (lua_State *L, int status);


#endif

