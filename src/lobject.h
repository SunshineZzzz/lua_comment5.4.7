/*
** $Id: lobject.h $
** Type definitions for Lua objects
** See Copyright Notice in lua.h
*/


#ifndef lobject_h
#define lobject_h


#include <stdarg.h>


#include "llimits.h"
#include "lua.h"


/*
** Extra types for collectable non-values
*/
#define LUA_TUPVAL	LUA_NUMTYPES  /* upvalues */
#define LUA_TPROTO	(LUA_NUMTYPES+1)  /* function prototypes */
#define LUA_TDEADKEY	(LUA_NUMTYPES+2)  /* removed keys in tables */



/*
** number of all possible types (including LUA_TNONE but excluding DEADKEY)
*/
#define LUA_TOTALTYPES		(LUA_TPROTO + 2)


/*
** tags for Tagged Values have the following use of bits:
** bits 0-3: actual tag (a LUA_T* constant)
** bits 4-5: variant bits
** bit 6: whether value is collectable
*/

// 生成一个变体类型
/* add variant bits to a type */
#define makevariant(t,v)	((t) | ((v) << 4))



/*
** Union of all Lua values
*/
// lua boolean 在类型字段中定义
typedef union Value {
  // 指向需要GC的对象
  struct GCObject *gc;    /* collectable objects */
  // light userdata
  void *p;         /* light userdata */
  // light c function
  lua_CFunction f; /* light C functions */
  // 整数
  lua_Integer i;   /* integer numbers */
  // 浮点数
  lua_Number n;    /* float numbers */
  /* not used, but may avoid warnings for uninitialized value */
  lu_byte ub;
} Value;


/*
** Tagged Values. This is the basic representation of values in Lua:
** an actual value plus a tag with its type.
*/

// value_ - 值
// 
// tt_ - 类型
// bit 0 ~ 3: 这里4个位，2的4次方即可以代表16个数值，用于存储变量的基本类型
// bit 4 ~ 5: 这里2个位，2的2次方即可以代表4个数值，用于存放类型变体，类型变体也属于它0到3位所对应的的基本类型
// bit     6: 这里1个位，2的1次方即可以代表2个数值，用于存储该变量是否可以垃圾回收
#define TValuefields	Value value_; lu_byte tt_

/*
* TValue
* {
*   Value value_
*   {
*       struct GCObject *gc;
*       {
*           &GCUnion.gc
*       }
*       void* p;
*       lua_CFunction f;
*       lua_Integer i;
*       lua_Number n; 
*   }
*   lu_byte tt_
* };
* 
* typedef struct GCObject 
* {
*   struct GCObject *next; lu_byte tt; lu_byte marked
* } GCObject;
*
* union GCUnion 
* {
*   GCObject gc;
*   struct TString ts;
*   struct Udata u;
*   union Closure cl;
*   struct Table h;
*   struct Proto p;
*   struct lua_State th;
*   struct UpVal upv;
* };
* 
* typedef struct GC对象 {
*   GCObject
*   其他定义
* }
*/
// Lua虚拟机中用于表示所有数据类型
typedef struct TValue {
  TValuefields;
} TValue;


// 取出value
#define val_(o)		((o)->value_)
#define valraw(o)	(val_(o))


// 获取类型tag
/* raw type tag of a TValue */
#define rawtt(o)	((o)->tt_)

// 获取基本类型
/* tag with no variants (bits 0-3) */
#define novariant(t)	((t) & 0x0F)

/* type tag of a TValue (bits 0-3 for tags + variant bits 4-5) */
// 基础类型+变体类型
#define withvariant(t)	((t) & 0x3F)
// 同上
#define ttypetag(o)	withvariant(rawtt(o))

// 获取TValue的基本类型
/* type of a TValue */
#define ttype(o)	(novariant(rawtt(o)))


/* Macros to test type */
// 是否是指定tag类型
#define checktag(o,t)		(rawtt(o) == (t))
// 是否是指定实际类型
#define checktype(o,t)		(ttype(o) == (t))


/* Macros for internal tests */

/* collectable object has the same tag as the original value */
#define righttt(obj)		(ttypetag(obj) == gcvalue(obj)->tt)

/*
** Any value being manipulated by the program either is non
** collectable, or the collectable object has the right tag
** and it is not dead. The option 'L == NULL' allows other
** macros using this one to be used where L is not available.
*/
#define checkliveness(L,obj) \
	((void)L, lua_longassert(!iscollectable(obj) || \
		(righttt(obj) && (L == NULL || !isdead(G(L),gcvalue(obj))))))


/* Macros to set values */

// 设置变体类型
/* set a value's tag */
#define settt_(o,t)	((o)->tt_=(t))


/* main macro to copy values (from 'obj2' to 'obj1') */
// copy obj2 to obj1
#define setobj(L,obj1,obj2) \
	{ TValue *io1=(obj1); const TValue *io2=(obj2); \
          io1->value_ = io2->value_; settt_(io1, io2->tt_); \
	  checkliveness(L,io1); lua_assert(!isnonstrictnil(io1)); }

/*
** Different types of assignments, according to source and destination.
** (They are mostly equal now, but may be different in the future.)
*/

/* from stack to stack */
#define setobjs2s(L,o1,o2)	setobj(L,s2v(o1),s2v(o2))
/* to stack (not from same stack) */
#define setobj2s(L,o1,o2)	setobj(L,s2v(o1),o2)
/* from table to same table */
#define setobjt2t	setobj
/* to new object */
#define setobj2n	setobj
/* to table */
#define setobj2t	setobj


/*
** Entries in a Lua stack. Field 'tbclist' forms a list of all
** to-be-closed variables active in this stack. Dummy entries are
** used when the distance between two tbc variables does not fit
** in an unsigned short. They are represented by delta==0, and
** their real delta is always the maximum value that fits in
** that field.
*/
// 栈中元素类型
typedef union StackValue {
  // 存储了数据的类型与值，它表示这个栈元素实际存储的数据
  TValue val;
  // 与数据清除to-be-closed相关
  struct {
    TValuefields;
    // 相邻tbc变量在栈中的距离
    unsigned short delta;
  } tbclist;
} StackValue;


/* index to stack elements */
typedef StackValue *StkId;


/*
** When reallocating the stack, change all pointers to the stack into
** proper offsets.
*/
typedef union {
  StkId p;  /* actual pointer */
  ptrdiff_t offset;  /* used while the stack is being reallocated */
} StkIdRel;


/* convert a 'StackValue' to a 'TValue' */
// 将StackValue转换为TValue
#define s2v(o)	(&(o)->val)



/*
** {==================================================================
** Nil
** ===================================================================
*/

/* Standard nil */
#define LUA_VNIL	makevariant(LUA_TNIL, 0)

// nil类型，并且变体标记为hashtable空值
/* Empty slot (which might be different from a slot containing nil) */
#define LUA_VEMPTY	makevariant(LUA_TNIL, 1)

/* Value returned for a key not found in a table (absent key) */
// 表中没有找到key时候返回的类型
#define LUA_VABSTKEY	makevariant(LUA_TNIL, 2)


/* macro to test for (any kind of) nil */
// 是否是nil
#define ttisnil(v)		checktype((v), LUA_TNIL)


/* macro to test for a standard nil */
#define ttisstrictnil(o)	checktag((o), LUA_VNIL)


// obj设置为nil
#define setnilvalue(obj) settt_(obj, LUA_VNIL)


// 是否是LUA_VABSTKEY类型
#define isabstkey(v)		checktag((v), LUA_VABSTKEY)


/*
** macro to detect non-standard nils (used only in assertions)
*/
#define isnonstrictnil(v)	(ttisnil(v) && !ttisstrictnil(v))


/*
** By default, entries with any kind of nil are considered empty.
** (In any definition, values associated with absent keys must also
** be accepted as empty.)
*/
// 是否为nil
#define isempty(v)		ttisnil(v)


/* macro defining a value corresponding to an absent key */
#define ABSTKEYCONSTANT		{NULL}, LUA_VABSTKEY


/* mark an entry as empty */
// hashtable nil变体
#define setempty(v)		settt_(v, LUA_VEMPTY)



/* }================================================================== */


/*
** {==================================================================
** Booleans
** ===================================================================
*/


// 布尔类型声明了两个类型变体，用第5位0代表false，1代表true。
// 布尔的基本类型值为LUA_TBOOLEAN为1，看前4位，与第5位的值，看得出来tt_ 若为（高位在左） 0000 0001，即为false；
// 若为 0001 0001，即为true。
#define LUA_VFALSE	makevariant(LUA_TBOOLEAN, 0)
#define LUA_VTRUE	makevariant(LUA_TBOOLEAN, 1)

// 是否是boolean类型
#define ttisboolean(o)		checktype((o), LUA_TBOOLEAN)
// lua boolean类型是否是false
#define ttisfalse(o)		checktag((o), LUA_VFALSE)
// lua boolean类型是否是true
#define ttistrue(o)		checktag((o), LUA_VTRUE)


// 是否是lua false，false或者nil
#define l_isfalse(o)	(ttisfalse(o) || ttisnil(o))


// obj设置为false
#define setbfvalue(obj)		settt_(obj, LUA_VFALSE)
// obj设置为true
#define setbtvalue(obj)		settt_(obj, LUA_VTRUE)

/* }================================================================== */


/*
** {==================================================================
** Threads
** ===================================================================
*/

// 线程变体类型
#define LUA_VTHREAD		makevariant(LUA_TTHREAD, 0)

#define ttisthread(o)		checktag((o), ctb(LUA_VTHREAD))

#define thvalue(o)	check_exp(ttisthread(o), gco2th(val_(o).gc))

// 将一个Table赋值给TValue 
#define setthvalue(L,obj,x) \
  { TValue *io = (obj); lua_State *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_VTHREAD)); \
    checkliveness(L,io); }

#define setthvalue2s(L,o,t)	setthvalue(L,s2v(o),t)

/* }================================================================== */


/*
** {==================================================================
** Collectable Objects
** ===================================================================
*/

/*
** Common Header for all collectable objects (in macro form, to be
** included in other objects)
*/
// 这是所有GC对象的公共头部
// next - 下一个GCObject指针
// tt - 类型
// marked - GC状态标记
#define CommonHeader	struct GCObject *next; lu_byte tt; lu_byte marked


/* Common type for all collectable objects */
// 所有GC对象的父类
typedef struct GCObject {
  CommonHeader;
} GCObject;


/* Bit mark for collectable types */
// GC对象标记
#define BIT_ISCOLLECTABLE	(1 << 6)

#define iscollectable(o)	(rawtt(o) & BIT_ISCOLLECTABLE)

/* mark a tag as collectable */
// 标记为GC对象
#define ctb(t)			((t) | BIT_ISCOLLECTABLE)

#define gcvalue(o)	check_exp(iscollectable(o), val_(o).gc)

#define gcvalueraw(v)	((v).gc)

#define setgcovalue(L,obj,x) \
  { TValue *io = (obj); GCObject *i_g=(x); \
    val_(io).gc = i_g; settt_(io, ctb(i_g->tt)); }

/* }================================================================== */


/*
** {==================================================================
** Numbers
** ===================================================================
*/

/* Variant tags for numbers */
// 整数类型变种
#define LUA_VNUMINT	makevariant(LUA_TNUMBER, 0)  /* integer numbers */
// 浮点类型变种
#define LUA_VNUMFLT	makevariant(LUA_TNUMBER, 1)  /* float numbers */

// 是否是number类型
#define ttisnumber(o)		checktype((o), LUA_TNUMBER)
// 是否是浮点类型
#define ttisfloat(o)		checktag((o), LUA_VNUMFLT)
// 是否是整数类型
#define ttisinteger(o)		checktag((o), LUA_VNUMINT)

#define nvalue(o)	check_exp(ttisnumber(o), \
	(ttisinteger(o) ? cast_num(ivalue(o)) : fltvalue(o)))
#define fltvalue(o)	check_exp(ttisfloat(o), val_(o).n)
#define ivalue(o)	check_exp(ttisinteger(o), val_(o).i)

#define fltvalueraw(v)	((v).n)
#define ivalueraw(v)	((v).i)

// 设置浮点数
#define setfltvalue(obj,x) \
  { TValue *io=(obj); val_(io).n=(x); settt_(io, LUA_VNUMFLT); }

// 修改浮点数
#define chgfltvalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisfloat(io)); val_(io).n=(x); }

// 设置整数
#define setivalue(obj,x) \
  { TValue *io=(obj); val_(io).i=(x); settt_(io, LUA_VNUMINT); }

// 修改整数
#define chgivalue(obj,x) \
  { TValue *io=(obj); lua_assert(ttisinteger(io)); val_(io).i=(x); }

/* }================================================================== */


/*
** {==================================================================
** Strings
** ===================================================================
*/

/* Variant tags for strings */
// 短字符串
#define LUA_VSHRSTR	makevariant(LUA_TSTRING, 0)  /* short strings */
// 长字符串
#define LUA_VLNGSTR	makevariant(LUA_TSTRING, 1)  /* long strings */

// 是否是字符串类型
#define ttisstring(o)		checktype((o), LUA_TSTRING)
// 字符串类型肯定是需要GC
// 是否是短字符串
#define ttisshrstring(o)	checktag((o), ctb(LUA_VSHRSTR))
// 是否是长字符串
#define ttislngstring(o)	checktag((o), ctb(LUA_VLNGSTR))

#define tsvalueraw(v)	(gco2ts((v).gc))

// 检测，将GCobject转成字符串
#define tsvalue(o)	check_exp(ttisstring(o), gco2ts(val_(o).gc))

// 将一个TString赋值给TValue 
#define setsvalue(L,obj,x) \
  { TValue *io = (obj); TString *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(x_->tt)); \
    checkliveness(L,io); }

/* set a string to the stack */
// 将string对象设置栈上
#define setsvalue2s(L,o,s)	setsvalue(L,s2v(o),s)

/* set a string to a new object */
#define setsvalue2n	setsvalue


/*
** Header for a string value.
*/
// 字符串类型
typedef struct TString {
  // GC对象的公共头部
  CommonHeader;
  // TString为⻓字符串时：当extra=0时表示该字符串未进⾏哈希运算；当extra=1时表示该字符串已经进⾏过哈希运算。
  // TString为短字符串时：当extra=0时表示它是普通字符串；当extra不为0时，它⼀般是lua保留字。
  lu_byte extra;  /* reserved words for short strings; "has hash" for longs */
  // 短字符串长度
  lu_byte shrlen;  /* length for short strings, 0xFF for long strings */
  // 字符串哈希值
  unsigned int hash;
  // 这是⼀个union结构。当TString为短字符串时，hnext域有效。当全局字符串表有其他字
  // 符串哈希冲突时，会将这些冲突的TString实例链接成单向链表，hnext的作⽤则是指向下⼀个
  // 对象在哪个位置。当TString为⻓字符串时，lnglen域⽣效，并且表示⻓字符串的⻓度也就是字
  // 符串Body的⻓度。
  union {
    size_t lnglen;  /* length for long strings */
    struct TString *hnext;  /* linked list for hash table */
  } u;
  // 这个是⽤来标记字符串Body起始位置的，配合shrlen或lnglen使⽤，能够找到字符串Body的结束位置在哪⾥
  char contents[1];
} TString;



/*
** Get the actual string (array of bytes) from a 'TString'. (Generic
** version and specialized versions for long and short strings.)
*/
// 获取内容
#define getstr(ts)	((ts)->contents)
#define getlngstr(ts)	check_exp((ts)->shrlen == 0xFF, (ts)->contents)
#define getshrstr(ts)	check_exp((ts)->shrlen != 0xFF, (ts)->contents)


/* get string length from 'TString *s' */
#define tsslen(s)  \
	((s)->shrlen != 0xFF ? (s)->shrlen : (s)->u.lnglen)

/* }================================================================== */


/*
** {==================================================================
** Userdata
** ===================================================================
*/


/*
** Light userdata should be a variant of userdata, but for compatibility
** reasons they are also different types.
*/
// Light User Data类型
#define LUA_VLIGHTUSERDATA	makevariant(LUA_TLIGHTUSERDATA, 0)

// Full User Data类型
#define LUA_VUSERDATA		makevariant(LUA_TUSERDATA, 0)

#define ttislightuserdata(o)	checktag((o), LUA_VLIGHTUSERDATA)
// 是否是Full User Data类型
#define ttisfulluserdata(o)	checktag((o), ctb(LUA_VUSERDATA))

// 返回Light User Data
#define pvalue(o)	check_exp(ttislightuserdata(o), val_(o).p)
// 返回Full User Data的gc指针
#define uvalue(o)	check_exp(ttisfulluserdata(o), gco2u(val_(o).gc))

#define pvalueraw(v)	((v).p)

// 将一个Light User Data赋值给TValue
#define setpvalue(obj,x) \
  { TValue *io=(obj); val_(io).p=(x); settt_(io, LUA_VLIGHTUSERDATA); }

// 将一个Full User Data赋值给TValue
#define setuvalue(L,obj,x) \
  { TValue *io = (obj); Udata *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_VUSERDATA)); \
    checkliveness(L,io); }


/* Ensures that addresses after this type are always fully aligned. */
// 用户自定义值
typedef union UValue {
  TValue uv;
  LUAI_MAXALIGN;  /* ensures maximum alignment for udata bytes */
} UValue;


/*
** Header for userdata with user values;
** memory area follows the end of this structure.
*/
// full user data
typedef struct Udata {
  CommonHeader;
  // 用户自定义值长度
  unsigned short nuvalue;  /* number of user values */
  // 用户数据长度
  size_t len;  /* number of bytes */
  // 元表
  struct Table *metatable;
  // GC相关的链表
  GCObject *gclist;
  // 用户自定义值内容
  UValue uv[1];  /* user values */
} Udata;
// 紧跟着的内存:
// C语言结构体占用内存


/*
** Header for userdata with no user values. These userdata do not need
** to be gray during GC, and therefore do not need a 'gclist' field.
** To simplify, the code always use 'Udata' for both kinds of userdata,
** making sure it never accesses 'gclist' on userdata with no user values.
** This structure here is used only to compute the correct size for
** this representation. (The 'bindata' field in its end ensures correct
** alignment for binary data following this header.)
*/
// 没有用户自定义值的full user data
// 为什么没有gclist域：
// Header for userdata with no user values. These userdata do not need
// to be gray during GC, and therefore do not need a 'gclist' field.
// 不会直接去创建这个对象，用来做偏移
typedef struct Udata0 {
  CommonHeader;
  unsigned short nuvalue;  /* number of user values */
  size_t len;  /* number of bytes */
  struct Table *metatable;
  union {LUAI_MAXALIGN;} bindata;
} Udata0;
// 紧跟着的内存:
// C语言结构体占用内存


/* compute the offset of the memory area of a userdata */
// 返回用户数据的内存区域的偏移量
#define udatamemoffset(nuv) \
	((nuv) == 0 ? offsetof(Udata0, bindata)  \
                    : offsetof(Udata, uv) + (sizeof(UValue) * (nuv)))

/* get the address of the memory block inside 'Udata' */
// 返回full user data中用户内存
#define getudatamem(u)	(cast_charp(u) + udatamemoffset((u)->nuvalue))

/* compute the size of a userdata */
// 
#define sizeudata(nuv,nb)	(udatamemoffset(nuv) + (nb))

/* }================================================================== */


/*
** {==================================================================
** Prototypes
** ===================================================================
*/

#define LUA_VPROTO	makevariant(LUA_TPROTO, 0)


/*
** Description of an upvalue for function prototypes
*/
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


/*
** Description of a local variable for function prototypes
** (used for debug information)
*/
// 局部变量调试信息
typedef struct LocVar {
  // 变量名称
  TString *varname;
  // 该局部变量存活的第一个指令序号
  int startpc;  /* first point where variable is active */
  // 该局部变量死亡的第一个指令序号
  int endpc;    /* first point where variable is dead */
} LocVar;


/*
** Associates the absolute line source for a given instruction ('pc').
** The array 'lineinfo' gives, for each instruction, the difference in
** lines from the previous instruction. When that difference does not
** fit into a byte, Lua saves the absolute line for that instruction.
** (Lua also saves the absolute line periodically, to speed up the
** computation of a line number: we can use binary search in the
** absolute-line array, but we must traverse the 'lineinfo' array
** linearly to compute a line.)
*/
// 绝对行号信息
typedef struct AbsLineInfo {
  // 对应的指令下标，Proto::code
  int pc;
  // 对应的绝对行号
  int line;
} AbsLineInfo;

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

/* }================================================================== */


/*
** {==================================================================
** Functions
** ===================================================================
*/

#define LUA_VUPVAL	makevariant(LUA_TUPVAL, 0)


/* Variant tags for functions */
// LUA_VLCL：Lua闭包
#define LUA_VLCL	makevariant(LUA_TFUNCTION, 0)  /* Lua closure */
// C函数指针，LUA源码是用C语言实现的，这个是一个纯粹的指向一个C语言函数的指针，不带UpValue
#define LUA_VLCF	makevariant(LUA_TFUNCTION, 1)  /* light C function */
// C语言闭包
#define LUA_VCCL	makevariant(LUA_TFUNCTION, 2)  /* C closure */

#define ttisfunction(o)		checktype(o, LUA_TFUNCTION)
#define ttisLclosure(o)		checktag((o), ctb(LUA_VLCL))
#define ttislcf(o)		checktag((o), LUA_VLCF)
#define ttisCclosure(o)		checktag((o), ctb(LUA_VCCL))
#define ttisclosure(o)         (ttisLclosure(o) || ttisCclosure(o))


#define isLfunction(o)	ttisLclosure(o)

#define clvalue(o)	check_exp(ttisclosure(o), gco2cl(val_(o).gc))
// GCobject转换成lua闭包
#define clLvalue(o)	check_exp(ttisLclosure(o), gco2lcl(val_(o).gc))
// 获取light c function
#define fvalue(o)	check_exp(ttislcf(o), val_(o).f)
#define clCvalue(o)	check_exp(ttisCclosure(o), gco2ccl(val_(o).gc))

#define fvalueraw(v)	((v).f)

// 将obj设置为一个闭包
#define setclLvalue(L,obj,x) \
  { TValue *io = (obj); LClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_VLCL)); \
    checkliveness(L,io); }

// 将将StackValue设置成一个闭包
#define setclLvalue2s(L,o,cl)	setclLvalue(L,s2v(o),cl)

#define setfvalue(obj,x) \
  { TValue *io=(obj); val_(io).f=(x); settt_(io, LUA_VLCF); }

#define setclCvalue(L,obj,x) \
  { TValue *io = (obj); CClosure *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_VCCL)); \
    checkliveness(L,io); }


/*
** Upvalues for Lua closures
*/
// UpVal类型有两种状态：分别是open打开和close关闭状态。
// 一个UpVal当它所属的那个函数返回之后（调用了return），
// 或者Lua运行堆栈发生改变，函数已经不处于合理堆栈下标的时候，
// 该函数所包含的UpVal即会切换到close状态。
typedef struct UpVal {
  CommonHeader;
  union {
    // 指向StkId
    TValue *p;  /* points to stack or to its own value */
    ptrdiff_t offset;  /* used while the stack is being reallocated */
  } v;
  union {
    // open时候生效
    struct {  /* (when open) */
      // 下一个
      struct UpVal *next;  /* linked list */
      // 上一个
      struct UpVal **previous;
    } open;
    // closed时候生效
    TValue value;  /* the value (when closed) */
  } u;
} UpVal;



// 闭包公共头
// nupvalues代表闭包中upvalues数组长度，
// gcList代表这个闭包结构体在垃圾清除的时候，
// 要清除包括upvalues在内的一系列可回收对象
#define ClosureHeader \
	CommonHeader; lu_byte nupvalues; GCObject *gclist

// C语言闭包
typedef struct CClosure {
  ClosureHeader;
  lua_CFunction f;
  TValue upvalue[1];  /* list of upvalues */
} CClosure;


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


typedef union Closure {
  CClosure c;
  LClosure l;
} Closure;


#define getproto(o)	(clLvalue(o)->p)

/* }================================================================== */


/*
** {==================================================================
** Tables
** ===================================================================
*/

// 没啥变种，就是重启一个名字
#define LUA_VTABLE	makevariant(LUA_TTABLE, 0)

// 是否为table类型
#define ttistable(o)		checktag((o), ctb(LUA_VTABLE))

#define hvalue(o)	check_exp(ttistable(o), gco2t(val_(o).gc))

// Table初始化创建一个TValue
#define sethvalue(L,obj,x) \
  { TValue *io = (obj); Table *x_ = (x); \
    val_(io).gc = obj2gco(x_); settt_(io, ctb(LUA_VTABLE)); \
    checkliveness(L,io); }

// 将table设置到StackValue
#define sethvalue2s(L,o,h)	sethvalue(L,s2v(o),h)


/*
** Nodes for Hash tables: A pack of two TValue's (key-value pairs)
** plus a 'next' field to link colliding entries. The distribution
** of the key's fields ('key_tt' and 'key_val') not forming a proper
** 'TValue' allows for a smaller size for 'Node' both in 4-byte
** and 8-byte alignments.
*/
// hashtable 节点
typedef union Node {
  // key
  struct NodeKey {
    // value
    TValuefields;  /* fields for value */
    // key类型
    lu_byte key_tt;  /* key type */
    // 哈希冲突下一个节点，相对于当前节点
    int next;  /* for chaining */
    // key value
    Value key_val;  /* key value */
  } u;
  // value，内存模型原因，可以直接访问hashtable key对应的value
  TValue i_val;  /* direct access to node's value as a proper 'TValue' */
} Node;


/* copy a value into a key */
// 从key中拷贝到node
#define setnodekey(L,node,obj) \
	{ Node *n_=(node); const TValue *io_=(obj); \
	  n_->u.key_val = io_->value_; n_->u.key_tt = io_->tt_; \
	  checkliveness(L,io_); }


/* copy a value from a key */
// 从node中拷贝出来key
#define getnodekey(L,obj,node) \
	{ TValue *io_=(obj); const Node *n_=(node); \
	  io_->value_ = n_->u.key_val; io_->tt_ = n_->u.key_tt; \
	  checkliveness(L,io_); }


/*
** About 'alimit': if 'isrealasize(t)' is true, then 'alimit' is the
** real size of 'array'. Otherwise, the real size of 'array' is the
** smallest power of two not smaller than 'alimit' (or zero iff 'alimit'
** is zero); 'alimit' is then used as a hint for #t.
*/

// 第8位为1的掩码
#define BITRAS		(1 << 7)
// 如果第8位为0，返回true，表示alimit是数组的实际长度。
// 如果第8位为1，返回false，表示alimit不是数组的实际长度。
#define isrealasize(t)		(!((t)->flags & BITRAS))
// 将flags的第8位设置为0，表示alimit是数组的实际长度
#define setrealasize(t)		((t)->flags &= cast_byte(~BITRAS))
// 将flags的第8位设置为1，表示alimit不是数组的实际长度，而是表数组部分的边界
#define setnorealasize(t)	((t)->flags |= BITRAS)


// lua table 实现
typedef struct Table {
  // GC公共部分
  CommonHeader;
  // flags用第8位0表达当前alimit是真正的数组长度
  // TM_EQ以前的元方法做了优化，如果第一次查找没找到，对应位置为1，下次直接就知道没找到了
  lu_byte flags;  /* 1<<p means tagmethod(p) is not present */
  // 哈希部分的长度，实际长度：2^lsizenode
  lu_byte lsizenode;  /* log2 of size of 'node' array */
  // alimit在大部份情况下为数组的长度（2次幂数），若不等于数组长度的时候，则数组长度为刚好比这个数大的下一个2次幂数，
  // 此时其数值为上次计算边界后缓存的返回值，而flags用第8位0表达当前alimit是真正的数组长度，1的话则不是，
  // 1的话就表示数组部分的边界
  // local test1 = {1, 2, 3,  [1] = 1 , [3] = 3 , [4] = 4 , [2] = 2}
  // 数组部分长度就是3，hash部分需要强制成2^n
  unsigned int alimit;  /* "limit" of 'array' array */
  // 数组
  TValue *array;  /* array part */
  // hashtable
  Node *node;
  // hashtable上一次空的结点位置
  Node *lastfree;  /* any free position is before this position */
  // 存放元表
  struct Table *metatable;
  // GC相关的链表
  GCObject *gclist;
} Table;


/*
** Macros to manipulate keys inserted in nodes
*/
// hashtable key类型
#define keytt(node)		((node)->u.key_tt)
// hashtable key值
#define keyval(node)		((node)->u.key_val)

#define keyisnil(node)		(keytt(node) == LUA_TNIL)
#define keyisinteger(node)	(keytt(node) == LUA_VNUMINT)
#define keyival(node)		(keyval(node).i)
// 节点key是否是段字符串类型
#define keyisshrstr(node)	(keytt(node) == ctb(LUA_VSHRSTR))
#define keystrval(node)		(gco2ts(keyval(node).gc))

// hashtable key值设置为nil
#define setnilkey(node)		(keytt(node) = LUA_TNIL)

#define keyiscollectable(n)	(keytt(n) & BIT_ISCOLLECTABLE)

#define gckey(n)	(keyval(n).gc)
#define gckeyN(n)	(keyiscollectable(n) ? gckey(n) : NULL)


/*
** Dead keys in tables have the tag DEADKEY but keep their original
** gcvalue. This distinguishes them from regular keys but allows them to
** be found when searched in a special way. ('next' needs that to find
** keys removed from a table during a traversal.)
*/
#define setdeadkey(node)	(keytt(node) = LUA_TDEADKEY)
#define keyisdead(node)		(keytt(node) == LUA_TDEADKEY)

/* }================================================================== */



/*
** 'module' operation for hashing (size is always a power of 2)
*/
#define lmod(s,size) \
	(check_exp((size&(size-1))==0, (cast_int((s) & ((size)-1)))))


// 计算2^x
#define twoto(x)	(1<<(x))
// 计算hashtable长度
#define sizenode(t)	(twoto((t)->lsizenode))


/* size of buffer for 'luaO_utf8esc' function */
#define UTF8BUFFSZ	8

LUAI_FUNC int luaO_utf8esc (char *buff, unsigned long x);
LUAI_FUNC int luaO_ceillog2 (unsigned int x);
LUAI_FUNC int luaO_rawarith (lua_State *L, int op, const TValue *p1,
                             const TValue *p2, TValue *res);
LUAI_FUNC void luaO_arith (lua_State *L, int op, const TValue *p1,
                           const TValue *p2, StkId res);
LUAI_FUNC size_t luaO_str2num (const char *s, TValue *o);
LUAI_FUNC int luaO_hexavalue (int c);
LUAI_FUNC void luaO_tostring (lua_State *L, TValue *obj);
LUAI_FUNC const char *luaO_pushvfstring (lua_State *L, const char *fmt,
                                                       va_list argp);
LUAI_FUNC const char *luaO_pushfstring (lua_State *L, const char *fmt, ...);
LUAI_FUNC void luaO_chunkid (char *out, const char *source, size_t srclen);


#endif

