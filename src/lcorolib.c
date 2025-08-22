/*
** $Id: lcorolib.c $
** Coroutine Library
** See Copyright Notice in lua.h
*/

#define lcorolib_c
#define LUA_LIB

#include "lprefix.h"


#include <stdlib.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"


// ��ȡЭ���߳�
static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}


/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
*/
// �ָ�(����)һ��Э��
// Lԭʼ�̣߳�coҪ�ָ����̣߳�narg�������
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  // ȷ��co�߳̿�������narg��������ջ�ռ�
  if (l_unlikely(!lua_checkstack(co, narg))) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  // ��ԭʼ�߳�Lջ�ϵ�narg�����������Ƶ�Ŀ��Э��co��ջ��
  /*
  * L:
  * [
  * ...(��һ��ci)
  * L->ci->func(coroutine.resume),
  * NL(new lua_state)
  * ����1
  * ...
  * ����narg
  * L->top(����λ��)
  * ]
  *
  * NL:
  * [
  * ...(��һ��ci)
  * NL->ci->func,
  * Э�̺���,
  * NL->top(����λ��)
  * ]
  */
  lua_xmove(L, co, narg);
  /*
  * L:
  * [
  * ...(��һ��ci)
  * L->ci->func(coroutine.resume),
  * NL(new lua_state)
  * L->top(����λ��)
  * ]
  *
  * NL:
  * [
  * ...(��һ��ci)
  * NL->ci->func,
  * Э�̺���,
  * ����1
  * ...
  * ����narg
  * NL->top(����λ��)
  * ]
  */
  // ����(�ָ�)Ŀ��Э��co��ִ��
  status = lua_resume(co, L, narg, &nres);
  if (l_likely(status == LUA_OK || status == LUA_YIELD)) {
    if (l_unlikely(!lua_checkstack(L, nres + 1))) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    // ��coջ�еķ���ֵ�ƶ���ԭʼ�߳�
    /*
    * L:
    * [
    * ...(��һ��ci)
    * L->ci->func(coroutine.resume),
    * NL(new lua_state)
    * NL����ֵ1
    * ...
    * NL����ֵnres
    * L->top(����λ��)
    * ]
    *
    * NL:
    * [
    * ...(��һ��ci)
    * NL->top(����λ��)
    * ]
    */
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    // ˵�������˴���
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}


// Э�̵�����(�ָ�)
static int luaB_coresume (lua_State *L) {
  // �õ�Э���߳�
  lua_State *co = getco(L);
  int r;
  // ���ģ��������߻ָ�Э��
  r = auxresume(L, co, lua_gettop(L) - 1);
  if (l_unlikely(r < 0)) {
    // ʧ���ˣ�ջ���Ѿ��д�����Ϣ�ˣ��ٴ�ѹ��false
    lua_pushboolean(L, 0);
    // false�ŵ�������Ϣǰ��
    // local bRet, val = coroutine.resume(co, 1)
    // OP_CALL,/* A B C R[A], ... ,R[A+C-2] := R[A](R[A+1], ... ,R[A+B-1]) */
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    // �ɹ��ˣ�ջ���Ѿ���r��Ҫ���ص�ֵ��
    lua_pushboolean(L, 1);
    // ͬ��
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}


// 
static int luaB_auxwrap (lua_State *L) {
  lua_State *co = lua_tothread(L, lua_upvalueindex(1));
  int r = auxresume(L, co, lua_gettop(L));
  if (l_unlikely(r < 0)) {  /* error? */
    int stat = lua_status(co);
    if (stat != LUA_OK && stat != LUA_YIELD) {  /* error in the coroutine? */
      stat = lua_closethread(co, L);  /* close its tbc variables */
      lua_assert(stat != LUA_OK);
      lua_xmove(co, L, 1);  /* move error message to the caller */
    }
    if (stat != LUA_ERRMEM &&  /* not a memory error and ... */
        lua_type(L, -1) == LUA_TSTRING) {  /* ... error object is a string? */
      luaL_where(L, 1);  /* add extra info, if available */
      lua_insert(L, -2);
      lua_concat(L, 2);
    }
    return lua_error(L);  /* propagate error */
  }
  return r;
}


// ����һ��Э��
/*
* ʹ�ø���ջ��
* L:
* [
* ...(��һ��ci)
* L->ci->func(coroutine.create),
* Э�̺���LClosure,
* NL��
* L->top(����λ��)
* ]
*
* NL:
* [
* NL->ci->func,
* Э�̺���LClosure,
* NL->top(����λ��)
* ]
*
* L���غ�
* L:
* [
* ...(��һ��ci)
* NL
* L->top(����λ��)
* ]
*/
static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  // ȷ����ǰջ�ĵ�һ��Ԫ����һ������
  luaL_checktype(L, 1, LUA_TFUNCTION);
  // ����һ���µ�lua�̲߳�ѹ��ջ
  NL = lua_newthread(L);
  // �ٴΰѺ���ѹ��ջ
  lua_pushvalue(L, 1);  /* move function to top */
  // �ٽ�ջ�еĺ����ƶ����µ��߳�
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}


// ����һ����װfunc��Э�̣�Ȼ�󷵻�һ�����ø�Э�̵ĺ���
static int luaB_cowrap (lua_State *L) {
  // �ȴ���һ��Э��
  luaB_cocreate(L);
  // ��Э����Ϊupvalue����luaB_auxwrap
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}


// yield
static int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}


// Э�̵�״̬
// Э����������ʱ��״̬
#define COS_RUN		0
// Э���Ѿ���������û�д���ʱ�����߷�������ʱ���������״̬
#define COS_DEAD	1
// Э�̹���ʱ��������һ��yield�ؼ���ʱ��Э��״̬���Ϊsuspended
#define COS_YIELD	2
// Э�̴�������״̬��û�й���Э��A�л���Э��B��A�ʹ���normal״̬��B��running״̬
#define COS_NORM	3


static const char *const statname[] =
  {"running", "dead", "suspended", "normal"};


// ״̬��ӦЭ��״̬�Ĺ�ϵ:״̬��LUA_YIELD ��ӦЭ�̵Ĺ���״̬
// ״̬��LUA_OK ��ӦЭ�̵�����,����,����״̬
static int auxstatus (lua_State *L, lua_State *co) {
  // ����ǵ�ǰ�������е�Э��
  if (L == co) return COS_RUN;
  else {
    switch (lua_status(co)) {
      // 
      case LUA_YIELD:
        // ��Ӧ�Ĺ���
        return COS_YIELD;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar))  /* does it have frames? */
          // 
          return COS_NORM;  /* it is running */
        else if (lua_gettop(co) == 0)
            // 
            return COS_DEAD;
        else
          // 
          return COS_YIELD;  /* initial state */
      }
      default:  /* some error occurred */
        return COS_DEAD;
    }
  }
}


// ��ȡЭ��״̬
static int luaB_costatus (lua_State *L) {
  lua_State *co = getco(L);
  lua_pushstring(L, statname[auxstatus(L, co)]);
  return 1;
}


static int luaB_yieldable (lua_State *L) {
  lua_State *co = lua_isnone(L, 1) ? L : getco(L);
  lua_pushboolean(L, lua_isyieldable(co));
  return 1;
}


static int luaB_corunning (lua_State *L) {
  int ismain = lua_pushthread(L);
  lua_pushboolean(L, ismain);
  return 2;
}


// Э�̹ر�
static int luaB_close (lua_State *L) {
  lua_State *co = getco(L);
  int status = auxstatus(L, co);
  switch (status) {
    case COS_DEAD: case COS_YIELD: {
      status = lua_closethread(co, L);
      if (status == LUA_OK) {
        lua_pushboolean(L, 1);
        return 1;
      }
      else {
        lua_pushboolean(L, 0);
        lua_xmove(co, L, 1);  /* move error message */
        return 2;
      }
    }
    default:  /* normal or running coroutine */
      return luaL_error(L, "cannot close a %s coroutine", statname[status]);
  }
}


// Э��Coroutine�����ĺ���
static const luaL_Reg co_funcs[] = {
  {"create", luaB_cocreate},
  {"resume", luaB_coresume},
  {"running", luaB_corunning},
  {"status", luaB_costatus},
  {"wrap", luaB_cowrap},
  {"yield", luaB_yield},
  {"isyieldable", luaB_yieldable},
  {"close", luaB_close},
  {NULL, NULL}
};



LUAMOD_API int luaopen_coroutine (lua_State *L) {
  luaL_newlib(L, co_funcs);
  return 1;
}

