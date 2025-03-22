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
// ָ��ջ�Ķ�����һ��++
#define api_incr_top(L)	{L->top.p++; \
			 api_check(L, L->top.p <= L->ci->top.p, \
					"stack overflow");}


/*
** If a call returns too many multiple returns, the callee may not have
** stack space to accommodate all results. In this case, this macro
** increases its stack space ('L->ci->top.p').
*/
// �����жϣ�
// (nres) <= LUA_MULTRET�������ؽ������nresΪ�ɱ�෵��ֵʱ����������
// L->ci->top.p < L->top.p����ǰ����֡��ջ��ָ�����ȫ��ջ��ָ�루˵�������÷�����ջ��ѹ���˸��෵��ֵ����
// ����������ǰ����֡��ջ��ָ�� L->ci->top.p ������ȫ��ջ�� L->top.p ��λ�ã���չ���÷���ջ�ռ䡣
#define adjustresults(L,nres) \
    { if ((nres) <= LUA_MULTRET && L->ci->top.p < L->top.p) \
	L->ci->top.p = L->top.p; }


/* Ensure the stack has at least 'n' elements */
// ȷ��ջ������Ҫ��n��Ԫ��
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

// ��鵱ǰ�����Ƿ����to-be-closed����
#define hastocloseCfunc(n)	((n) < LUA_MULTRET)

/* Map [-1, inf) (range of 'nresults') into (-inf, -2] */
// �ӷ�Χ[-1, inf)ӳ�䵽��Χ(-inf, -2]
#define codeNresults(n)		(-(n) - 3)
// �ӷ�Χ(-inf, -2]�����ԭʼ��Χ[-1, inf)
#define decodeNresults(n)	(-(n) - 3)

#endif
