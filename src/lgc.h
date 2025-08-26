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
// 传播阶段：当一轮完整的GC将要开始了，就会从GCSpause暂停阶段切换到GCSpropagate传播阶段。
// 进入这个阶段之前，会对global_State全局状态机中的某几个字段作为染色传播的根结点进行标记染色。
// 进入传播阶段后，由上述几个根结点开始，根据引用关系通过深度优先遍历，尝试对所有可达的对象进行标记染色。
#define GCSpropagate	0
// 原子阶段：当传播阶段对所有可达对象染色结束后，GC则从GCSpropagate阶段切换到GCSenteratomic原子阶段。
// 原子阶段不可打断，不可分步执行，原子阶段执行完成后即会立即进入到下一个阶段。
// 增量式算法的分步执行会存在对象引用关系在运行过程发生变化，在标记阶段中，引用关系的变化可能导致Lua的一致性原则被破坏，
// 一致性原则即：黑色对象不能指向白色对象。Lua在标记阶段使用前进屏障和后退屏障来修正颜色标记保持一致性原则。
// 在原子阶段，会结合屏障对对象进行正确的颜色标记；另外，也会处理弱引用关系Table，即带有__mode元方法的table，
// 让其中的键弱、值弱、键值弱Table里面的元素能得到正确的标记处理；最后，会把未标记但带析构器的对象进行标记，
// 以防在清除阶段被清除，并添加到g->tobefnz链表中。
#define GCSenteratomic	1
#define GCSatomic	2
// allgc清除阶段：GCSenteratomic原子阶段执行完成后，就会立即进入到GCSswpallgc阶段，从此阶段开始，
// 标记清除算法就算是进入到了清除阶段。
// 不带有__gc元方法的对象视为无析构器对象，此类对象在创建的时候会插入到global_State的allgc链表中，
// 本阶段的逻辑就是把allgc链表中未标记的元素进行清除。
#define GCSswpallgc	3
// 析构器对象清除阶段：GCSswpallgc阶段对allgc链表中的所有元素处理完成后，即会进入到GCSswpfinobj析构器对象清除阶段。
// 带有__gc元方法的对象视为有析构器对象，此类对象在创建完成后，会从allgc链表移出，并移入到finobj链表，
// 本阶段即为对这些带析构器的对象进行标记清除处理。
#define GCSswpfinobj	4
// 将要释放的带析构器的对象清除阶段：GCSswpfinobj阶段对所有finobj对象处理完成后，即会进入到GCSswptobefnz阶段。
// 与上个阶段一样，同样是处理带析构器的对象，不同的地方是，上个阶段GCSswpfinobj阶段处理的对象存在于finobj链表，
// 此链表中的对象都是可达、存活、已标记状态的；而本阶段处理的对象存在tobefnz链表中，当一个带析构器对象未标记将要被清除时，
// 就会从finobj链表移出，并移入到tobefnz链表，等待GC后续阶段调用它们的析构器方法，本阶段就是把tobefnz链表中的元素标记进行清除。
#define GCSswptobefnz	5
// 清除结束阶段：GCSswptobefnz阶段对所有tobefnz对象处理完成后，即会进入到GCSswpend阶段。
// 本阶段是对清除阶段工作进行收尾，代码较少，暂时只是对全局存储短字符串的stringtable哈希表根据当前哈希表的元素使用数量，
// 判断并决定是否要对它的部分内存进行回收。
#define GCSswpend	6
// 析构器调用阶段：GCSswpend执行完成后，即会立即进入到GCScallfin阶段。
// 本阶段会处理将要被释放的带析构器的对象，即tobefnz链表中的对象，会依次调用它们的析构器函数。
// 调用结束后，则把他们的析构器标记清除，然后当作普通对象重新插入到allgc链表中。
// 若它们在下一轮完整GC中仍然未标记，则在下一轮的时候会把它们当作普通的对象进行直接清除与内存释放。
// 当tobefnz链表中所有对象的析构器都调用完毕后，GC则由GCScallfin阶段回到GCSpause暂停阶段。一轮完整的GC宣告结束。
#define GCScallfin	7
// 暂停阶段：当GC未开始或完整的一轮全部结束后，就会处于这个阶段
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

// 是否是标记阶段
// 在标记清除算法的整个标记阶段，Lua会坚守一个原则，就是“一致性原则”或者“不变性原则”。
// 这个原则的内容就是：黑色对象不能引用白色对象
// 标记阶段有可能打破这个规则：
// 1）创建了新的对象，新对象会被初始化为白色，但引用这个新对象的却是一个黑色对象；
// 2）修改一个仍未被标记的对象（白色）的引用者（父结点）为一个黑色对象；
// 这些情况都会导致黑色对象引用白色对象，打破一致性原则。若一致性原则在被打破后没有被修复，
// 则这个白色对象由于父结点黑色的关系是没有机会在本轮GC的标记阶段被处理标记到的，
// 从而导致一直保持白色并在清除阶段被清除。这并不是我们期待的结果，为了解决这个问题，
// Lua引入了屏障（barrier）这个概念。
// 屏障的作用就是为了防止一致性原则被破坏的，它是通过重新修改颜色的方式来实现的。
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
