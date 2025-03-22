/*
** $Id: lapi.h $
** Auxiliary functions from Lua API
** See Copyright Notice in lua.h
*/

#ifndef lapi_h
#define lapi_h


#include "llimits.h"
#include "lstate.h"


/* Increments 'L->top.p', checking for stack overflows */
// 指向栈的顶部下一个++
#define api_incr_top(L)	{L->top.p++; \
			 api_check(L, L->top.p <= L->ci->top.p, \
					"stack overflow");}


/*
** If a call returns too many multiple returns, the callee may not have
** stack space to accommodate all results. In this case, this macro
** increases its stack space ('L->ci->top.p').
*/
// 条件判断：
// (nres) <= LUA_MULTRET：当返回结果数量nres为可变多返回值时触发调整。
// L->ci->top.p < L->top.p：当前调用帧的栈顶指针低于全局栈顶指针（说明被调用方已向栈中压入了更多返回值）。
// 操作：将当前调用帧的栈顶指针 L->ci->top.p 提升到全局栈顶 L->top.p 的位置，扩展调用方的栈空间。
#define adjustresults(L,nres) \
    { if ((nres) <= LUA_MULTRET && L->ci->top.p < L->top.p) \
	L->ci->top.p = L->top.p; }


/* Ensure the stack has at least 'n' elements */
// 确定栈中至少要有n个元素
#define api_checknelems(L,n) \
	api_check(L, (n) < (L->top.p - L->ci->func.p), \
			  "not enough elements in the stack")


/*
** To reduce the overhead of returning from C functions, the presence of
** to-be-closed variables in these functions is coded in the CallInfo's
** field 'nresults', in a way that functions with no to-be-closed variables
** with zero, one, or "all" wanted results have no overhead. Functions
** with other number of wanted results, as well as functions with
** variables to be closed, have an extra check.
*/

// 检查当前函数是否包含to-be-closed变量
#define hastocloseCfunc(n)	((n) < LUA_MULTRET)

/* Map [-1, inf) (range of 'nresults') into (-inf, -2] */
// 从范围[-1, inf)映射到范围(-inf, -2]
#define codeNresults(n)		(-(n) - 3)
// 从范围(-inf, -2]解码回原始范围[-1, inf)
#define decodeNresults(n)	(-(n) - 3)

#endif
