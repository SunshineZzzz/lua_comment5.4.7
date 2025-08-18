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
// ԭ��������ö��
typedef enum {
  // __index Ԫ���������ٷ��ʣ�
  TM_INDEX,
  // __newindex Ԫ���������ٷ��ʣ�
  TM_NEWINDEX,
  // __gc Ԫ���������ٷ��ʣ�
  TM_GC,
  // __mode Ԫ���������ٷ��ʣ�
  TM_MODE,
  // __len Ԫ���������ٷ��ʣ�
  TM_LEN,
  // __eq Ԫ���������ٷ��ʣ����һ�����п��ٷ������Ե�Ԫ������
  TM_EQ,  /* last tag method with fast access */
  // __add Ԫ����
  TM_ADD,
  // __sub Ԫ����
  TM_SUB,
  // __mul Ԫ����
  TM_MUL,
  // __mod Ԫ����
  TM_MOD,
  // __pow Ԫ����
  TM_POW,
  // __div Ԫ����
  TM_DIV,
  // __idiv Ԫ����
  TM_IDIV,
  // __band Ԫ����
  TM_BAND,
  // __bor Ԫ����
  TM_BOR,
  // __bxor Ԫ����
  TM_BXOR,
  // __shl Ԫ����
  TM_SHL,
  // __shr Ԫ����
  TM_SHR,
  // __unm Ԫ����
  TM_UNM,
  // __bnot Ԫ����
  TM_BNOT,
  // __lt Ԫ����
  TM_LT,
  // __le Ԫ����
  TM_LE,
  // __concat Ԫ����
  TM_CONCAT,
  // __call Ԫ����
  TM_CALL,
  // __metatable Ԫ����
  TM_CLOSE,
  // Ԫ����������
  TM_N		/* number of elements in the enum */
} TMS;


/*
** Mask with 1 in all fast-access methods. A 1 in any of these bits
** in the flag of a (meta)table means the metatable does not have the
** corresponding metamethod field. (Bit 7 of the flag is used for
** 'isrealasize'.)
*/
// Ԫ��flags�ĳ�ʼֵ
// ��8λΪ0����alimit������ʵ�ʴ�С��ʶ,�������Ϊ1����������ʵ�ʴ�С��ʶ
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
// �Ƿ���Ԫ����
#define notm(tm)	ttisnil(tm)


// ��ȡԪ����
// �������һ���Ż�,����һ�β��ұ��е�ĳ��Ԫ��������û���ҵ�ʱ,�ὫTable�е�flags��Ա��Ӧ��λ����λ����,������һ���������Ҹñ���ͬ����Ԫ����ʱ�����λ�Ѿ�Ϊ1,��ôֱ�ӷ���NULL
#define gfasttm(g,et,e) ((et) == NULL ? NULL : \
  ((et)->flags & (1u<<(e))) ? NULL : luaT_gettm(et, e, (g)->tmname[e]))

// ͬ�ϣ�ֻ������һ��������lua_State
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
