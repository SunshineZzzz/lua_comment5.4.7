1. 初始化lua状态机
```C
lua_State* lua = luaL_newstate();

// 指向主线程
g->l_registry->array[0] = L
// 全局变量
g->l_registry->array[1] = _G = {}
```

2. 加载并注册Lua的所有标准库到Lua的全局表中
```C
luaL_openlibs(lua);

g->l_registry->array[1] = _G = {
    "_G" = _G,
    "_VERSION" = LUA_VERSION,
    "package" = {...}
    "io" = {...}
    ...
    print = func,
    assert = func,
    ...
    "full user data 的 tname" = 原表{__name=tname}
    ...
}

g->l_registry["_LOADED"] = {
    "_G" = {...},
    "package" = {...}
    "io" = {...}
    ...
}
```

3. 词法语法分析
```C
luaY_parser
```
编译chunk后对应的LClosure在栈中

4. 准备CallInfo
```C
luaD_precall
```

5. 循环执行指令 
```
luaV_execute
```

6. 一些关键解释
- chunk: 它是一段能够被lua解释器编译运行的代码，它可以是一个lua脚本文件，或者是在交互模式中，输入的包含一段代码的字符串。

- ```LClosure```: lua闭包函数，chunk经过词语法分析后，生成这个对象在lua栈中。Proto和upvals组成。把chunk作为一个最顶层lua闭包函数。
```C
// Lua闭包
typedef struct LClosure {
  ClosureHeader;
  // 描述lua函数，还是c语言闭包最简单，就一个函数指针即可
  struct Proto *p;
  // 外部数据
  // 在嵌套函数中，内层函数如果仍然有被引用处于有效状态，而外层函数已经没有被引用了已经无效了，
  // 此时Lua支持在保留内层函数的情况下，对外层函数进行清除，从而可以清理掉外层函数引用的非当前函数UpValue用途以外的大量数据内存
  // 尽管外层函数被清除了，Lua仍然可以保持内层函数用到的UpValue值的有效性
  // UpValue有open与close两种状态，当外层函数被清除的时候，UpValue会有一个由open状态切换到close状态的过程，
  // 会对数据进行一定的处理
  UpVal *upvals[1];  /* list of upvalues */
} LClosure;
```

- ```LClosure::UpVal```: lua闭包定义，把chunk作为一个最顶层lua闭包函数，该闭包默认带有一个UpValue，这个UpValue的变量名为"_ENV"，它指向Lua虚拟机的全局变量表，即_G表，可以理解为_G表即为当前Lua文件中代码的运行环境(env)。事实上，每一个Lua闭包它们第一个UpValue值都是_ENV。
```C
// 加载Lua代码块但不运行
LUA_API int lua_load (lua_State *L, lua_Reader reader, void *data,
                      const char *chunkname, const char *mode) {
  ZIO z;
  int status;
  lua_lock(L);
  if (!chunkname) chunkname = "?";
  luaZ_init(L, &z, reader, data);
  status = luaD_protectedparser(L, &z, chunkname, mode);
  if (status == LUA_OK) {  /* no errors? */
    LClosure *f = clLvalue(s2v(L->top.p - 1));  /* get new function */
    if (f->nupvalues >= 1) {  /* does it have an upvalue? */
      // env参数不传的话默认就会被设置为_G表
      /* get global table from registry */
      const TValue *gt = getGtable(L);
      /* set global table as 1st upvalue of 'f' (may be LUA_ENV) */
      setobj(L, f->upvals[0]->v.p, gt);
      luaC_barrier(L, f->upvals[0], gt);
    }
  }
  lua_unlock(L);
  return status;
}
```

- ```Proto```: lua函数原型，主要放的就是字节码(指令) 。其中struct Proto **p定义了函数内部定义的函数对应的函数原型。内部的函数在执行指令的时候才会创建LClosure。
```C
/*
** Function Prototypes
*/
// lua函数原型，存放字节码(opcode)
// 字节码是⼀种能够被虚拟机识别的中间代码。⼀些解释型语⾔，
// 能够通过它们的编译器将源代码编译成字节码，再交给虚拟机去执⾏。
// ⼀个Lua函数对应⼀个Proto实例
typedef struct Proto {
  // GC公共定义
  CommonHeader;
  // 函数固定参数的个数
  lu_byte numparams;  /* number of fixed (named) parameters */
  // 是否为可变参函数
  lu_byte is_vararg;
  // Proto实例所对应函数栈空间的⼤⼩
  lu_byte maxstacksize;  /* number of registers needed by this function */
  // UpValue的数量
  int sizeupvalues;  /* size of 'upvalues' */
  // 常量的数量
  int sizek;  /* size of 'k' */
  // 指令码数量
  int sizecode;
  // lineinfo数组的容量
  int sizelineinfo;
  // **p容量
  int sizep;  /* size of 'p' */
  // 局部变量的数量
  int sizelocvars;
  // *abslineinfo容量
  int sizeabslineinfo;  /* size of 'abslineinfo' */
  // 函数的开始行
  int linedefined;  /* debug information  */
  // 函数的结束行
  int lastlinedefined;  /* debug information  */
  // 常量所存储在的数组
  TValue *k;  /* constants used by the function */
  // 指令码数组
  Instruction *code;  /* opcodes */
  // 函数内部又定义了的其它函数，所以说Lua的函数支持嵌套定义
  struct Proto **p;  /* functions defined inside the function */
  // UpValue的信息
  // 用于描述UpValue并定位到具体某个UpValue的地址，不存储实际的UpValue数值或引用，
  // 而是通过一定的方式指向LClosure的upvalues数组。
  // 解析器解析Lua代码的时候会生成这个UpValue描述信息，并用于生成指令，
  // 而执行器运行的时候可以通过该描述信息方便快速地构建出真正的UpValue数组；
  Upvaldesc *upvalues;  /* upvalue information */
  // 新增指令下标 对应 新增指令和上一个指令行号差值
  ls_byte *lineinfo;  /* information about source lines (debug information) */
  // 存储绝对行号信息
  AbsLineInfo *abslineinfo;  /* idem */
  // 局部变量的描述
  // 这里的局部变量locvars与sizelocvars仅用于解析器解析和运行时信息调试，实际执行器运行是不必要的，
  // 因为对应的指令已经完成生成，对应的变量也都会被放到相应位置的运行堆栈上，不需要在Proto这里进行额外的存储
  LocVar *locvars;  /* information about local variables (debug information) */
  // 该函数的源代码的字符串表示
  TString  *source;  /* used for debug information */
  // GC相关链表
  GCObject *gclist;
} Proto;
```

- ```Proto::Upvaldesc```: 用于描述UpValue并定位到具体某个UpValue的地址，不存储实际的UpValue数值或引用，而是通过一定的方式指向LClosure的upvalues数组。
```C
// 函数原型的上值描述
typedef struct Upvaldesc {
  // 上值名称
  TString *name;  /* upvalue name (for debug information) */
  // 本函数的upvalue，是否指向外层函数的栈(不是则指向外层函数的某个upvalue值)
  // 1表示该上值在外层函数的栈上，0表示该上值引⽤的是外层函数的上值
  lu_byte instack;  /* whether it is in stack (register) */
  // 如果instack为1，idx表示该上值是外层函数栈上第idx位置的值
  // 如果instack为0，idx表示该上值是位于外层函数upval[idx]的位置
  lu_byte idx;  /* index of upvalue (in stack or in outer function's list) */
  // 上值类型
  lu_byte kind;  /* kind of corresponding variable */
} Upvaldesc;
```
![alt text](../img/upvalue1.png)

- ```Open Upvalue```和```Closed Upvalue```: UpVal类型有两种状态：分别是open打开和close关闭状态。一个UpVal当它所属的那个函数返回之后（调用了return），或者Lua运行堆栈发生改变，函数已经不处于合理堆栈下标的时候，该函数所包含的UpVal即会切换到close状态。
  
脚本完成编译时的状态:

![alt text](../img/upvalue2.png)

执行函数aaa():

**函数bbb和函数ccc的第2个（var2 upvalue）UpVal\* 指针指向了同一个UpVal\* 实例。下图中所示的，包含的值，指向LuaStack中的两个Upvalue，此时是Open Upvalue。**

![alt text](../img/upvalue3.png)

执行到调用第一个bbb函数的时候:

**在aaa函数执行完毕时，他们的UpVal*实例，会进行close操作，此时是Close Upvalue。**

![alt text](../img/upvalue4.png)


- ```CallInfo```: lua闭包与lua运行栈的中间桥梁，其中StkIdRel func函数地址指针，指向CallInfo对应的Lua闭包在运行栈中的位置。CallInfo初始化的时候赋值，运行时不变。其中StkIdRel	top指向插入数据后会到达的最高的Lua运行栈的位置，CallInfo初始化的时候赋值，运行时不变。其中struct CallInfo *previous, *next调用链。其中const Instruction *savedpc用于记录当前虚拟机执行器执行到当前函数的哪条指令。
```C
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
```
![alt text](../img/callinfo1.png)

- 指令介绍:
```C
/*===========================================================================
  We assume that instructions are unsigned 32-bit integers.
  All instructions have an opcode in the first 7 bits.
  Instructions can have the following formats:

        3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0
        1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
iABC          C(8)     |      B(8)     |k|     A(8)      |   Op(7)     |
iABx                Bx(17)               |     A(8)      |   Op(7)     |
iAsBx              sBx (signed)(17)      |     A(8)      |   Op(7)     |
iAx                           Ax(25)                     |   Op(7)     |
isJ                           sJ (signed)(25)            |   Op(7)     |

  A signed argument is represented in excess K: the represented value is
  the written unsigned value minus K, where K is half the maximum for the
  corresponding unsigned argument.
===========================================================================*/


// 指令模式
// iABC参数上限：4个，8位的A（2的8次方即最多能存储256个数），8位的B，8位的C，1位（2的1次方即最多能存储2个数）的k
// iABC是最常用的指令模式
// 
// iABx参数上限：2个，8位的A，17位无符号的Bx（Bx，2的17次方即最多能存储131072个数）
// 
// iAsBx参数上限：2个，8位的A，17位有符号的Bx（sBx，2的17次方即最多能存储131072个数）
// 
// iAx参数上限：1个，25位无符号的Ax（Ax，2的25次方即最多能存储33554432个数）
// 
// isJ参数上限：1个，25位有符号的sJ（sJ，2的25次方即最多能存储33554432个数）
// 目前，唯一使用该指令模式的只有跳转指令OP_JMP；所以该指令模式名字'isJ'及参数'sJ'都带有字母‘J’(Jump，跳转)，
// 就是为了更加直观地知道这个指令模式对应的是跳转功能
enum OpMode {iABC, iABx, iAsBx, iAx, isJ};  /* basic instruction formats */
```

1. overview.lua指令
   
![alt text](../img/overview.png)
