1. lua中需要GC的对象
```C
/*
** basic types
*/
#define LUA_TNONE		(-1)

#define LUA_TNIL		0
#define LUA_TBOOLEAN		1
#define LUA_TLIGHTUSERDATA	2
#define LUA_TNUMBER		3
// 这个下面都是需要GC
#define LUA_TSTRING		4
#define LUA_TTABLE		5
#define LUA_TFUNCTION		6
#define LUA_TUSERDATA		7
#define LUA_TTHREAD		8

#define LUA_NUMTYPES		9

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


// 模拟父类向子类转换的中间态，主要是目的就是转换
// 这里都是能被GC回收的对象
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

// 其他需要GC的类型不展示
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

// value_ - 值
// 
// tt_ - 类型
// bit 0 ~ 3: 这里4个位，2的4次方即可以代表16个数值，用于存储变量的基本类型
// bit 4 ~ 5: 这里2个位，2的2次方即可以代表4个数值，用于存放类型变体，类型变体也属于它0到3位所对应的的基本类型
// bit     6: 这里1个位，2的1次方即可以代表2个数值，用于存储该变量是否可以垃圾回收
#define TValuefields	Value value_; lu_byte tt_

typedef struct TValue {
  TValuefields;
} TValue;
```

2. GC对象如何被分配
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
```

3. 