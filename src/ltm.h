/*
** $Id: ltm.h $
** Tag methods
** See Copyright Notice in lua.h
*/

#ifndef ltm_h
#define ltm_h


#include "lobject.h"


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER TM" and "ORDER OP"
*/
// 原方法类型枚举
typedef enum {
  // __index 元方法（快速访问）
  TM_INDEX,
  // __newindex 元方法（快速访问）
  TM_NEWINDEX,
  // __gc 元方法（快速访问）
  TM_GC,
  // __mode 元方法（快速访问）
  TM_MODE,
  // __len 元方法（快速访问）
  TM_LEN,
  // __eq 元方法（快速访问，最后一个具有快速访问特性的元方法）
  TM_EQ,  /* last tag method with fast access */
  // __add 元方法
  TM_ADD,
  // __sub 元方法
  TM_SUB,
  // __mul 元方法
  TM_MUL,
  // __mod 元方法
  TM_MOD,
  // __pow 元方法
  TM_POW,
  // __div 元方法
  TM_DIV,
  // __idiv 元方法
  TM_IDIV,
  // __band 元方法
  TM_BAND,
  // __bor 元方法
  TM_BOR,
  // __bxor 元方法
  TM_BXOR,
  // __shl 元方法
  TM_SHL,
  // __shr 元方法
  TM_SHR,
  // __unm 元方法
  TM_UNM,
  // __bnot 元方法
  TM_BNOT,
  // __lt 元方法
  TM_LT,
  // __le 元方法
  TM_LE,
  // __concat 元方法
  TM_CONCAT,
  // __call 元方法
  TM_CALL,
  // __metatable 元方法
  TM_CLOSE,
  // 元方法的总数
  TM_N		/* number of elements in the enum */
} TMS;


/*
** Mask with 1 in all fast-access methods. A 1 in any of these bits
** in the flag of a (meta)table means the metatable does not have the
** corresponding metamethod field. (Bit 7 of the flag is used for
** 'isrealasize'.)
*/
// 元表flags的初始值
// 第8位为0代表alimit是数组实际大小标识,否则如果为1代表不是数组实际大小标识
// 00000000 00000000 00000000 00000000 ~
// ->
// 11111111 11111111 11111111 11111111 << 6
// ->
// 11111111 11111111 11111111 11000000 ~
// ->
// 00000000 00000000 00000000 00111111
#define maskflags	(~(~0u << (TM_EQ + 1)))


/*
** Test whether there is no tagmethod.
** (Because tagmethods use raw accesses, the result may be an "empty" nil.)
*/
// 是否有元方法
#define notm(tm)	ttisnil(tm)


// 获取元方法
// 这里会做一个优化,当第一次查找表中的某个元方法并且没有找到时,会将Table中的flags成员对应的位做置位操作,这样下一次再来查找该表中同样的元方法时如果该位已经为1,那么直接返回NULL
#define gfasttm(g,et,e) ((et) == NULL ? NULL : \
  ((et)->flags & (1u<<(e))) ? NULL : luaT_gettm(et, e, (g)->tmname[e]))

// 同上，只不过第一个参数是lua_State
#define fasttm(l,et,e)	gfasttm(G(l), et, e)

#define ttypename(x)	luaT_typenames_[(x) + 1]

LUAI_DDEC(const char *const luaT_typenames_[LUA_TOTALTYPES];)


LUAI_FUNC const char *luaT_objtypename (lua_State *L, const TValue *o);

LUAI_FUNC const TValue *luaT_gettm (Table *events, TMS event, TString *ename);
LUAI_FUNC const TValue *luaT_gettmbyobj (lua_State *L, const TValue *o,
                                                       TMS event);
LUAI_FUNC void luaT_init (lua_State *L);

LUAI_FUNC void luaT_callTM (lua_State *L, const TValue *f, const TValue *p1,
                            const TValue *p2, const TValue *p3);
LUAI_FUNC void luaT_callTMres (lua_State *L, const TValue *f,
                            const TValue *p1, const TValue *p2, StkId p3);
LUAI_FUNC void luaT_trybinTM (lua_State *L, const TValue *p1, const TValue *p2,
                              StkId res, TMS event);
LUAI_FUNC void luaT_tryconcatTM (lua_State *L);
LUAI_FUNC void luaT_trybinassocTM (lua_State *L, const TValue *p1,
       const TValue *p2, int inv, StkId res, TMS event);
LUAI_FUNC void luaT_trybiniTM (lua_State *L, const TValue *p1, lua_Integer i2,
                               int inv, StkId res, TMS event);
LUAI_FUNC int luaT_callorderTM (lua_State *L, const TValue *p1,
                                const TValue *p2, TMS event);
LUAI_FUNC int luaT_callorderiTM (lua_State *L, const TValue *p1, int v2,
                                 int inv, int isfloat, TMS event);

LUAI_FUNC void luaT_adjustvarargs (lua_State *L, int nfixparams,
                                   struct CallInfo *ci, const Proto *p);
LUAI_FUNC void luaT_getvarargs (lua_State *L, struct CallInfo *ci,
                                              StkId where, int wanted);


#endif
