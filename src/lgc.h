/*
** $Id: lgc.h $
** Garbage Collector
** See Copyright Notice in lua.h
*/

#ifndef lgc_h
#define lgc_h


#include "lobject.h"
#include "lstate.h"

/*
** Collectable objects may have one of three colors: white, which means
** the object is not marked; gray, which means the object is marked, but
** its references may be not marked; and black, which means that the
** object and all its references are marked.  The main invariant of the
** garbage collector, while marking objects, is that a black object can
** never point to a white one. Moreover, any gray object must be in a
** "gray list" (gray, grayagain, weak, allweak, ephemeron) so that it
** can be visited again before finishing the collection cycle. (Open
** upvalues are an exception to this rule.)  These lists have no meaning
** when the invariant is not being enforced (e.g., sweep phase).
*/


/*
** Possible states of the Garbage Collector
*/
// GC�׶�
// ɨ��
#define GCSpropagate	0
// 
#define GCSenteratomic	1
#define GCSatomic	2
// 
#define GCSswpallgc	3
// 
#define GCSswpfinobj	4
// 
#define GCSswptobefnz	5
// 
#define GCSswpend	6
// 
#define GCScallfin	7
// ������һ������GC�������Էֲ�����
#define GCSpause	8


#define issweepphase(g)  \
	(GCSswpallgc <= (g)->gcstate && (g)->gcstate <= GCSswpend)


/*
** macro to tell when main invariant (white objects cannot point to black
** ones) must be kept. During a collection, the sweep
** phase may break the invariant, as objects turned white may point to
** still-black objects. The invariant is restored when sweep ends and
** all objects are white again.
*/

// �Ƿ��Ǳ�ǽ׶�
// �ڱ������㷨��������ǽ׶Σ�Lua�����һ��ԭ�򣬾��ǡ�һ����ԭ�򡱻��ߡ�������ԭ�򡱡�
// ���ԭ������ݾ��ǣ���ɫ���������ð�ɫ����
// ��ǽ׶��п��ܴ����������
// 1���������µĶ����¶���ᱻ��ʼ��Ϊ��ɫ������������¶����ȴ��һ����ɫ����
// 2���޸�һ����δ����ǵĶ��󣨰�ɫ���������ߣ�����㣩Ϊһ����ɫ����
// ��Щ������ᵼ�º�ɫ�������ð�ɫ���󣬴���һ����ԭ����һ����ԭ���ڱ����ƺ�û�б��޸���
// �������ɫ�������ڸ�����ɫ�Ĺ�ϵ��û�л����ڱ���GC�ı�ǽ׶α������ǵ��ģ�
// �Ӷ�����һֱ���ְ�ɫ��������׶α�������Ⲣ���������ڴ��Ľ����Ϊ�˽��������⣬
// Lua���������ϣ�barrier��������
// ���ϵ����þ���Ϊ�˷�ֹһ����ԭ���ƻ��ģ�����ͨ�������޸���ɫ�ķ�ʽ��ʵ�ֵġ�
#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


/*
** some useful bit tricks
*/
// ������λ��ز���
#define resetbits(x,m)		((x) &= cast_byte(~(m)))
#define setbits(x,m)		((x) |= (m))
#define testbits(x,m)		((x) & (m))
#define bitmask(b)		(1<<(b))
#define bit2mask(b1,b2)		(bitmask(b1) | bitmask(b2))
#define l_setbit(x,b)		setbits(x, bitmask(b))
#define resetbit(x,b)		resetbits(x, bitmask(b))
#define testbit(x,b)		testbits(x, bitmask(b))


/*
** Layout for bit use in 'marked' field. First three bits are
** used for object "age" in generational mode. Last bit is used
** by tests.
*/
// 8bit,�ӵ���λ��ʼʹ�ã���7λû�õ���������λ���˷ִ�ʹ����
// ��ɫ��������ɫ����
// ��0��bitmask(3)��00001000
#define WHITE0BIT	3  /* object is white (type 0) */
// ��1��bitmask(4)��00010000
#define WHITE1BIT	4  /* object is white (type 1) */
// ��ɫ��bitmask(5)��00100000
#define BLACKBIT	5  /* object is black */
// ������ǣ��������������Ķ���ӵ�����������ǣ������ͷ�֮ǰ��Ҫ�ȵ���__GCԪ������bitmask(6)��01000000
#define FINALIZEDBIT	6  /* object has been marked for finalization */

#define TESTBIT		7


// ��ɫ bitmask(3)|bitmask(4) 00011000
#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)


// �Ƿ��ǰ�ɫ
#define iswhite(x)      testbits((x)->marked, WHITEBITS)
// �Ƿ��Ǻ�ɫ
#define isblack(x)      testbit((x)->marked, BLACKBIT)
// �Ƿ��ǻ�ɫ��3��4��5��Ӧ��bitλ������0����Ȼ���ǻ�ɫ
#define isgray(x)  /* neither white nor black */  \
	(!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))

// �Ƿ����__GCԭ�����ı�/full user data
#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)

// ��ȡ�ǵ�ǰ��ɫ
#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
#define isdeadm(ow,m)	((m) & (ow))
// ��ǰ�ף�����GC�������
// �����ף�����GC��Ҫ����
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)

// �޸ĵ�ǰ��ɫΪ�ǵ�ǰ��
#define changewhite(x)	((x)->marked ^= WHITEBITS)
// ���Ϊ��ɫ
#define nw2black(x)  \
	check_exp(!iswhite(x), l_setbit((x)->marked, BLACKBIT))

// �Ƿ�Ϊ��ɫ
#define luaC_white(g)	cast_byte((g)->currentwhite & WHITEBITS)


/* object age in generational mode */
// �ִ�GC���䶨��
#define G_NEW		0	/* created in current cycle */
#define G_SURVIVAL	1	/* created in previous cycle */
#define G_OLD0		2	/* marked old by frw. barrier in this cycle */
#define G_OLD1		3	/* first full cycle as old */
#define G_OLD		4	/* really old object (not to be visited) */
#define G_TOUCHED1	5	/* old object touched this cycle */
#define G_TOUCHED2	6	/* old object touched in previous cycle */

#define AGEBITS		7  /* all age bits (111) */

#define getage(o)	((o)->marked & AGEBITS)
#define setage(o,a)  ((o)->marked = cast_byte(((o)->marked & (~AGEBITS)) | a))
#define isold(o)	(getage(o) > G_SURVIVAL)

#define changeage(o,f,t)  \
	check_exp(getage(o) == (f), (o)->marked ^= ((f)^(t)))


/* Default Values for GC parameters */
#define LUAI_GENMAJORMUL         100
#define LUAI_GENMINORMUL         20

/* wait memory to double before starting new cycle */
#define LUAI_GCPAUSE    200

/*
** some gc parameters are stored divided by 4 to allow a maximum value
** up to 1023 in a 'lu_byte'.
*/
#define getgcparam(p)	((p) * 4)
#define setgcparam(p,v)	((p) = (v) / 4)

// 
#define LUAI_GCMUL      100

/* how much to allocate before next GC step (log2) */
#define LUAI_GCSTEPSIZE 13      /* 8 KB */


/*
** Check whether the declared GC mode is generational. While in
** generational mode, the collector can go temporarily to incremental
** mode to improve performance. This is signaled by 'g->lastatomic != 0'.
*/
// �Ƿ��Ƿִ�GCģʽ
#define isdecGCmodegen(g)	(g->gckind == KGC_GEN || g->lastatomic != 0)


/*
** Control when GC is running:
*/
#define GCSTPUSR	1  /* bit true when GC stopped by user */
#define GCSTPGC		2  /* bit true when GC stopped by itself */
#define GCSTPCLS	4  /* bit true when closing Lua state */
#define gcrunning(g)	((g)->gcstp == 0)


/*
** Does one step of collection when debt becomes positive. 'pre'/'pos'
** allows some adjustments to be done only when needed. macro
** 'condchangemem' is used only for heavy tests (forcing a full
** GC cycle on every opportunity)
*/
// ծ�����0����Ҫ����GC����
#define luaC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

/* more often than not, 'pre'/'pos' are empty */
// ����ʱ�����������ڴ���GC�����ʱ�򣬾�����Բ����
#define luaC_checkGC(L)		luaC_condGC(L,(void)0,(void)0)


#define luaC_objbarrier(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? \
	luaC_barrier_(L,obj2gco(p),obj2gco(o)) : cast_void(0))

#define luaC_barrier(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrier(L,p,gcvalue(v)) : cast_void(0))

#define luaC_objbarrierback(L,p,o) (  \
	(isblack(p) && iswhite(o)) ? luaC_barrierback_(L,p) : cast_void(0))

#define luaC_barrierback(L,p,v) (  \
	iscollectable(v) ? luaC_objbarrierback(L, p, gcvalue(v)) : cast_void(0))

LUAI_FUNC void luaC_fix (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_freeallobjects (lua_State *L);
LUAI_FUNC void luaC_step (lua_State *L);
LUAI_FUNC void luaC_runtilstate (lua_State *L, int statesmask);
LUAI_FUNC void luaC_fullgc (lua_State *L, int isemergency);
LUAI_FUNC GCObject *luaC_newobj (lua_State *L, int tt, size_t sz);
LUAI_FUNC GCObject *luaC_newobjdt (lua_State *L, int tt, size_t sz,
                                                 size_t offset);
LUAI_FUNC void luaC_barrier_ (lua_State *L, GCObject *o, GCObject *v);
LUAI_FUNC void luaC_barrierback_ (lua_State *L, GCObject *o);
LUAI_FUNC void luaC_checkfinalizer (lua_State *L, GCObject *o, Table *mt);
LUAI_FUNC void luaC_changemode (lua_State *L, int newmode);


#endif
