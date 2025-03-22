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


// 获取协程栈
static lua_State *getco (lua_State *L) {
  lua_State *co = lua_tothread(L, 1);
  luaL_argexpected(L, co, 1, "thread");
  return co;
}


/*
** Resumes a coroutine. Returns the number of results for non-error
** cases or -1 for errors.
*/
// 启动(恢复)一个协程
// L原始线程，co要启动的线程，narg参数个数
static int auxresume (lua_State *L, lua_State *co, int narg) {
  int status, nres;
  // 确保co线程可以放入narg个参数的栈空间
  if (l_unlikely(!lua_checkstack(co, narg))) {
    lua_pushliteral(L, "too many arguments to resume");
    return -1;  /* error flag */
  }
  // 将原始线程L栈顶的narg个参数移动到目标协程co的栈中
  lua_xmove(L, co, narg);
  // 恢复目标协程co的执行
  status = lua_resume(co, L, narg, &nres);
  if (l_likely(status == LUA_OK || status == LUA_YIELD)) {
    if (l_unlikely(!lua_checkstack(L, nres + 1))) {
      lua_pop(co, nres);  /* remove results anyway */
      lua_pushliteral(L, "too many results to resume");
      return -1;  /* error flag */
    }
    lua_xmove(co, L, nres);  /* move yielded values */
    return nres;
  }
  else {
    lua_xmove(co, L, 1);  /* move error message */
    return -1;  /* error flag */
  }
}


// 协程的启动(恢复)
static int luaB_coresume (lua_State *L) {
  // 拿到协程线程
  lua_State *co = getco(L);
  int r;
  // 核心：启动或者恢复协程
  r = auxresume(L, co, lua_gettop(L) - 1);
  if (l_unlikely(r < 0)) {
    lua_pushboolean(L, 0);
    lua_insert(L, -2);
    return 2;  /* return false + error message */
  }
  else {
    lua_pushboolean(L, 1);
    lua_insert(L, -(r + 1));
    return r + 1;  /* return true + 'resume' returns */
  }
}


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


// 创建一个协程
static int luaB_cocreate (lua_State *L) {
  lua_State *NL;
  // 确保当前栈第一个元素是一个函数
  luaL_checktype(L, 1, LUA_TFUNCTION);
  // 创建一个新的lua线程，并且压入栈
  NL = lua_newthread(L);
  // 再次把函数放入栈顶
  lua_pushvalue(L, 1);  /* move function to top */
  // 再将栈顶函数弹出到新的线程
  lua_xmove(L, NL, 1);  /* move function from L to NL */
  return 1;
}


// 创建一个封装func的协程，然后返回一个调用该协程的函数
static int luaB_cowrap (lua_State *L) {
  // 先创建一个协程
  luaB_cocreate(L);
  // 把协程对象函数当做luaB_auxwrap的一个upvalue
  lua_pushcclosure(L, luaB_auxwrap, 1);
  return 1;
}


// 挂起状态
static int luaB_yield (lua_State *L) {
  return lua_yield(L, lua_gettop(L));
}


// 协程的状态
// 当协程运行时
#define COS_RUN		0
// 当协程运行完要执行的代码时或者在运行代码时发生了错误(error)
#define COS_DEAD	1
// 当一个协程刚被创建时或遇到函数中的yield关键字时,它的状态变为suspended
#define COS_YIELD	2
// 当协程处于活跃状态，但没有被运行
// （这意味着程序在运行另一个协程，比如从协程A中唤醒协程B，此时A就处于正常状态，因为当前运行的是协程B）
#define COS_NORM	3


static const char *const statname[] =
  {"running", "dead", "suspended", "normal"};


// 状态对应协程状态的关系:状态机的LUA_YIELD 对应协程的挂起状态
// 状态机的LUA_OK 对应协程的正常,死亡,挂起状态
static int auxstatus (lua_State *L, lua_State *co) {
  if (L == co) return COS_RUN;
  else {
    switch (lua_status(co)) {
      case LUA_YIELD:
        return COS_YIELD;
      case LUA_OK: {
        lua_Debug ar;
        if (lua_getstack(co, 0, &ar))  /* does it have frames? */
          return COS_NORM;  /* it is running */
        else if (lua_gettop(co) == 0)
            return COS_DEAD;
        else
          return COS_YIELD;  /* initial state */
      }
      default:  /* some error occurred */
        return COS_DEAD;
    }
  }
}


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


// 协程销毁
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


// 协程Coroutine相关的函数
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

