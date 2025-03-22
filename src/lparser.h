/*
** $Id: lparser.h $
** Lua Parser
** See Copyright Notice in lua.h
*/

#ifndef lparser_h
#define lparser_h

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/*
** Expression and variable descriptor.
** Code generation for variables and expressions can be delayed to allow
** optimizations; An 'expdesc' structure describes a potentially-delayed
** variable/expression. It has a description of its "main" value plus a
** list of conditional jumps that can also produce its value (generated
** by short-circuit operators 'and'/'or').
*/

/* kinds of variables/expressions */
// 表达式类型
typedef enum {
  VVOID,  /* when 'expdesc' describes the last expression of a list,
             this kind means an empty list (so, no expression) */
  // nil常量
  VNIL,  /* constant nil */
  // true常量
  VTRUE,  /* constant true */
  // false常量
  VFALSE,  /* constant false */
  // 常量字面量，表达式需要存储在函数原型常量表中(Proto::k)，expdesc::info是k的下标
  VK,  /* constant in 'k'; info = index of constant in 'k' */
  // float常量，expdesc的nval字段表示
  VKFLT,  /* floating constant; nval = numerical float value */
  // integer常量，expdesc的ival字段表示
  VKINT,  /* integer constant; ival = numerical integer value */
  // string常量，expdesc的strval字段表示
  VKSTR,  /* string constant; strval = TString address;
             (string is fixed by the lexer) */
  // 不需要重定向，已经将表达式定向到了寄存器中，info记录是寄存器下标
  VNONRELOC,  /* expression has its value in a fixed register;
                 info = result register */
  // 局部变量，var.ridx就是寄存器的下标，var.vidx就是
  // fs->ls->dyd->actvar.arr[fs->firstlocal + vidx]就是这里的vidx
  VLOCAL,  /* local variable; var.ridx = register index;
              var.vidx = relative index in 'actvar.arr'  */
  // 上值，info记录在Proto::upvalues中的下标
  VUPVAL,  /* upvalue variable; info = index of upvalue in 'upvalues' */
  // 前面已经优化编译好的常量，actvar.arr中保存了变量，info使用的绝对索引
  VCONST,  /* compile-time <const> variable;
              info = absolute index in 'actvar.arr'  */
  // t[动态]，t是表，动态是指表达式的值需要在运行时计算
  // ind.t = t 寄存器下标
  // ind.idx = 动态寄存器下标
  VINDEXED,  /* indexed variable;
                ind.t = table register;
                ind.idx = key's R index */
  // t[k]，t是表，k在常量表中字符串，
  // ind.t = t upvalue中下标
  // ind.idx = k在常量表中的下标
  VINDEXUP,  /* indexed upvalue;
                ind.t = table upvalue;
                ind.idx = key's K index */
  // t[k]，t是表，k就是idx的整数
  // ind.t = t 寄存器下标
  // ind.idx = 就是整数
  VINDEXI, /* indexed variable with constant integer;
                ind.t = table register;
                ind.idx = key's value */
  // t.k，t是表，k在常量表中字符串，
  // ind.t = t 寄存器下标
  // ind.idx = k在常量表中的下标
  VINDEXSTR, /* indexed variable with literal string;
                ind.t = table register;
                ind.idx = key's K index */
  // 跳转表达式
  // info 指令下标
  VJMP,  /* expression is a test/comparison;
            info = pc of corresponding jump instruction */
  // ⽬标寄存器未定，info指向的是指令下标
  VRELOC,  /* expression can put result in any register;
              info = instruction pc */
  // 函数调用，info指向的是指令下标
  VCALL,  /* expression is a function call; info = instruction pc */
  // 可变参数，info指向的是指令下标
  VVARARG  /* vararg expression; info = instruction pc */
} expkind;


#define vkisvar(k)	(VLOCAL <= (k) && (k) <= VINDEXSTR)
#define vkisindexed(k)	(VINDEXED <= (k) && (k) <= VINDEXSTR)


// 表达式描述信息
typedef struct expdesc {
  // 表达式类型
  expkind k;
  union {
    // 表达式是integer常量
    lua_Integer ival;    /* for VKINT */
    // 表达式是double常量
    lua_Number nval;  /* for VKFLT */
    // 表达式string字面量
    TString *strval;  /* for VKSTR */
    // 通用
    int info;  /* for generic use */
    // 索引变量
    struct {  /* for indexed variables */
      // 寄存器下标或者整数或者常量表
      short idx;  /* index (R or "long" K) */
      // 类型
      lu_byte t;  /* table (register or upvalue) */
    } ind;
    // 本地变量
    struct {  /* for local variables */
      // 持有变量的寄存器
      lu_byte ridx;  /* register holding the variable */
      // fs->ls->dyd->actvar.arr[fs->firstlocal + vidx]就是这里的vidx
      unsigned short vidx;  /* compiler index (in 'actvar.arr')  */
    } var;
  } u;
  // 当表达式为真时跳转的目标地址列表
  int t;  /* patch list of 'exit when true' */
  // 当表达式为假时跳转的目标地址列表
  int f;  /* patch list of 'exit when false' */
} expdesc;


/* kinds of variables */
// 普通local
#define VDKREG		0  /* regular */
// 具有const属性local
#define RDKCONST	1  /* constant */
// 具有close属性local
#define RDKTOCLOSE	2  /* to-be-closed */
// 上面constant确定可以编译成“编译器常量”
#define RDKCTC		3  /* compile-time constant */

/* description of an active local variable */
// local变量定义
typedef union Vardesc {
  struct {
    // 下面的常量值
    TValuefields;  /* constant value (if it is a compile-time constant) */
    // 变量类型
    lu_byte kind;
    // 持有变量的寄存器
    lu_byte ridx;  /* register holding the variable */
    // Proto::locvars数组中变量的索引
    short pidx;  /* index of the variable in the Proto's 'locvars' array */
    // 变量名
    TString *name;  /* variable name */
  } vd;
  // 用于存储常量值
  TValue k;  /* constant value (if any) */
} Vardesc;



/* description of pending goto statements and label statements */
// 描述单个标签或未匹配的goto
typedef struct Labeldesc {
  // 标签名称
  TString *name;  /* label identifier */
  // 标签在字节码中的位置，指令下标
  // 未匹配(前向跳转)：newgotoentry(ls, name, line, luaK_jump(fs)); -> 
  // return codesJ(fs, OP_JMP, NO_JUMP, 0); -> luaK_code ->  f->code[fs->pc++] = i; fs->pc - 1;
  // 标签：int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs)); -> fs->lasttarget = fs->pc; return fs->pc;
  // 
  // 
  int pc;  /* position in code */
  // 标签出现的行号
  int line;  /* line where it appeared */
  // 在该位置的活动变量数量，用于确保跳转时局部变量的作用域正确
  lu_byte nactvar;  /* number of active variables in that position */
  // 是否需要关闭上值（upvalue），如果跳转跨越了局部变量的作用域，则需要生成OP_CLOSE指令 
  lu_byte close;  /* goto that escapes upvalues */
} Labeldesc;


/* list of labels or gotos */
// 存储标签或goto列表
typedef struct Labellist {
  // 数组
  Labeldesc *arr;  /* array */
  // 当前使用的条目数量
  int n;  /* number of entries in use */
  // 数组大小
  int size;  /* array size */
} Labellist;


/* dynamic structures used by the parser */
// 语法分析的动态数据
typedef struct Dyndata {
  // 记录1ocal变量的信息
  struct {  /* list of all active local variables */
    Vardesc *arr;
    // 当前用到个数
    int n;
    // arr列表大小
    int size;
  } actvar;
  // 存储未匹配标签的信息
  Labellist gt;  /* list of pending gotos */
  // 存储已声明的标签信息
  Labellist label;   /* list of active labels */
} Dyndata;


/* control of blocks */
struct BlockCnt;  /* defined in lparser.c */


/* state needed to generate code for a given function */
// ⼀个Lua函数则对应⼀个FuncState结构。FuncState结构实例只在编译期存在
// 函数编译期间的上下文信息
typedef struct FuncState {
  // 存放字节码
  Proto *f;  /* current function header */
  // 上一个函数编译上下文信息
  struct FuncState *prev;  /* enclosing function */
  // 对应的词法状态机
  struct LexState *ls;  /* lexical state */
  // 当前解析的block
  struct BlockCnt *bl;  /* chain of current blocks */
  // 下一个code下标，Proto::code
  // 是当前函数的程序计数器，表示下一条将要生成的指令位置
  int pc;  /* next position to code (equivalent to 'ncode') */
  // 最近一次被标记为跳转目标的位置
  // 防止编译器对连续指令进行错误优化，确保控制流逻辑的正确性
  int lasttarget;   /* 'label' of last 'jump label' */
  // 上一个指令所在行号
  int previousline;  /* last line that was saved in 'lineinfo' */
  // Proto::k的下标
  int nk;  /* number of elements in 'k' */
  // Proto::**p数量
  int np;  /* number of elements in 'p' */
  // 对应Proto::abslineinfo下标，绝对行号下标
  int nabslineinfo;  /* number of elements in 'abslineinfo' */
  // 第一个可用local变量的位置
  int firstlocal;   /* index of first local var (in Dyndata array) */
  // 当函数中，第一个标签语句在全局已声明的标签中的位置
  int firstlabel;  /* index of first label (in 'dyd->label->arr') */
  // 局部变量调试信息的数量，Proto::locvars
  short ndebugvars;  /* number of elements in 'f->locvars' */
  // 活跃变量总数，其本质就是local变量的总数。
  // 是一个计数器，表示当前作用域中声明的局部变量数量。
  // 但它并不直接反映这些变量是否被分配到寄存器中。
  // 例如，某些变量可能是编译时常量（RDKCTC），它们不会占用寄存器
  lu_byte nactvar;  /* number of active local variables */
  // upvalues下标
  // 对应到这里fs->f->upvalues取值
  lu_byte nups;  /* number of upvalues */
  // 空闲的栈下标，入栈+1，出栈-1
  lu_byte freereg;  /* first free register */
  // 相对行号使用次数
  lu_byte iwthabs;  /* instructions issued since last absolute line info */
  // 为1，自身某个变量作为上值被其他作用域引用
  // 函数结束时，编译器生成关闭指令，确保该上值的生命周期得到正确管理
  lu_byte needclose;  /* function needs to close upvalues when returning */
} FuncState;


LUAI_FUNC int luaY_nvarstack (FuncState *fs);
LUAI_FUNC LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                                 Dyndata *dyd, const char *name, int firstchar);


#endif
