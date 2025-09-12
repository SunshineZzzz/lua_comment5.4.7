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
// GC阶段
// 扫描
#define GCSpropagate	0
// 原子
#define GCSenteratomic	1
#define GCSatomic	2
// 清除
#define GCSswpallgc	3
// 清除g->finobj链表，黑色的带有析构器的对象颜色改成当前白
#define GCSswpfinobj	4
// 清除g->tobefnz链表，黑色的带有析构器的对象颜色改成当前白
#define GCSswptobefnz	5
// 清除结束
#define GCSswpend	6
// 析构器调用
#define GCScallfin	7
// 开启新一轮增量GC，不可以分步进行
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

// 当前GC阶段是否需要保持"强三色不变性",黑色对象绝对不能直接引用任何白色对象​​,只要保证这个，
// 就保证不会出新建白色对象被错误清除。
// 在原子阶段之后，GC 进入清扫阶段（GCSsweep），在这个阶段，GC 会故意破坏三色不变式，将一些对象变成白色以便回收。
#define keepinvariant(g)	((g)->gcstate <= GCSatomic)


/*
** some useful bit tricks
*/
// 二进制位相关操作
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
// 8bit,从第三位开始使用，第7位没用到，后面三位给了分代使用了
// 三色标记清除颜色定义
// 白0，bitmask(3)，00001000
#define WHITE0BIT	3  /* object is white (type 0) */
// 白1，bitmask(4)，00010000
#define WHITE1BIT	4  /* object is white (type 1) */
// 黑色，bitmask(5)，00100000
#define BLACKBIT	5  /* object is black */
// 析构标记，定义了析构器的对象拥有这个析构标记，代表释放之前需要先调用__GC元方法，bitmask(6)，01000000
#define FINALIZEDBIT	6  /* object has been marked for finalization */

#define TESTBIT		7


// 白色 bitmask(3)|bitmask(4) 00011000
#define WHITEBITS	bit2mask(WHITE0BIT, WHITE1BIT)


// 是否是白色
#define iswhite(x)      testbits((x)->marked, WHITEBITS)
// 是否是黑色
#define isblack(x)      testbit((x)->marked, BLACKBIT)
// 是否是灰色，3，4，5对应的bit位都不是0，自然就是灰色
#define isgray(x)  /* neither white nor black */  \
	(!testbits((x)->marked, WHITEBITS | bitmask(BLACKBIT)))

// 是否具有__GC原方法的表/full user data
#define tofinalize(x)	testbit((x)->marked, FINALIZEDBIT)

// 获取非当前白色
#define otherwhite(g)	((g)->currentwhite ^ WHITEBITS)
#define isdeadm(ow,m)	((m) & (ow))
// 当前白，本次GC不会回收
// 其他白，本次GC需要回收
#define isdead(g,v)	isdeadm(otherwhite(g), (v)->marked)

// 修改当前白色为非当前白
#define changewhite(x)	((x)->marked ^= WHITEBITS)
// 标记为黑色
#define nw2black(x)  \
	check_exp(!iswhite(x), l_setbit((x)->marked, BLACKBIT))

// 是否为白色
#define luaC_white(g)	cast_byte((g)->currentwhite & WHITEBITS)


/* object age in generational mode */
// 分代GC年龄定义
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
// 是否是分代GC模式
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
// 债务大于0就需要进行GC操作
#define luaC_condGC(L,pre,pos) \
	{ if (G(L)->GCdebt > 0) { pre; luaC_step(L); pos;}; \
	  condchangemem(L,pre,pos); }

/* more often than not, 'pre'/'pos' are empty */
// 调用时机基本上是在创建GC对象的时候，具体可以查代码
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
