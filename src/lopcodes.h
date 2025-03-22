/*
** $Id: lopcodes.h $
** Opcodes for Lua virtual machine
** See Copyright Notice in lua.h
*/

#ifndef lopcodes_h
#define lopcodes_h

#include "llimits.h"


/*===========================================================================
  We assume that instructions are unsigned 32-bit integers.
  All instructions have an opcode in the first 7 bits.
  Instructions can have the following formats:

        3 3 2 2 2 2 2 2 2 2 2 2 1 1 1 1 1 1 1 1 1 1 0 0 0 0 0 0 0 0 0 0
        1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0 9 8 7 6 5 4 3 2 1 0
iABC          C(8)     |      B(8)     |k|     A(8)      |   Op(7)     |
iABx                Bx(17)               |     A(8)      |   Op(7)     |
iAsBx              sBx (signed)(17)      |     A(8)      |   Op(7)     |
iAx                           Ax(25)                     |   Op(7)     |
isJ                           sJ (signed)(25)            |   Op(7)     |

  A signed argument is represented in excess K: the represented value is
  the written unsigned value minus K, where K is half the maximum for the
  corresponding unsigned argument.
===========================================================================*/


// 指令模式
// iABC参数上限：4个，8位的A（2的8次方即最多能存储256个数），8位的B，8位的C，1位（2的1次方即最多能存储2个数）的k
// iABC是最常用的指令模式
// 
// iABx参数上限：2个，8位的A，17位无符号的Bx（Bx，2的17次方即最多能存储131072个数）
// 
// iAsBx参数上限：2个，8位的A，17位有符号的Bx（sBx，2的17次方即最多能存储131072个数）
// 
// iAx参数上限：1个，25位无符号的Ax（Ax，2的25次方即最多能存储33554432个数）
// 
// isJ参数上限：1个，25位有符号的sJ（sJ，2的25次方即最多能存储33554432个数）
// 目前，唯一使用该指令模式的只有跳转指令OP_JMP；所以该指令模式名字'isJ'及参数'sJ'都带有字母‘J’(Jump，跳转)，
// 就是为了更加直观地知道这个指令模式对应的是跳转功能
enum OpMode {iABC, iABx, iAsBx, iAx, isJ};  /* basic instruction formats */


/*
** size and position of opcode arguments.
*/
// C参数大小8位
#define SIZE_C		8
// B参数大小8位
#define SIZE_B		8
// Bx17位
#define SIZE_Bx		(SIZE_C + SIZE_B + 1)
// A参数大小8位
#define SIZE_A		8
// Ax参数大小25位
#define SIZE_Ax		(SIZE_Bx + SIZE_A)
// sJ参数大小25位
#define SIZE_sJ		(SIZE_Bx + SIZE_A)

// 指令长度
#define SIZE_OP		7

// 指令起始位置
#define POS_OP		0

// A参数起始位置，7
#define POS_A		(POS_OP + SIZE_OP)
// k参数起始位置，15
#define POS_k		(POS_A + SIZE_A)
// B参数起始位置，16
#define POS_B		(POS_k + 1)
// C参数起始位置，24
#define POS_C		(POS_B + SIZE_B)

// Bx参数起始位置，15
#define POS_Bx		POS_k

// Ax参数起始位置，7
#define POS_Ax		POS_A

// sJ参数起始位置，7
#define POS_sJ		POS_A


/*
** limits for opcode arguments.
** we use (signed) 'int' to manipulate most arguments,
** so they must fit in ints.
*/

/* Check whether type 'int' has at least 'b' bits ('b' < 32) */
#define L_INTHASBITS(b)		((UINT_MAX >> ((b) - 1)) >= 1)


#if L_INTHASBITS(SIZE_Bx)
#define MAXARG_Bx	((1<<SIZE_Bx)-1)
#else
#define MAXARG_Bx	MAX_INT
#endif

#define OFFSET_sBx	(MAXARG_Bx>>1)         /* 'sBx' is signed */


#if L_INTHASBITS(SIZE_Ax)
#define MAXARG_Ax	((1<<SIZE_Ax)-1)
#else
#define MAXARG_Ax	MAX_INT
#endif

#if L_INTHASBITS(SIZE_sJ)
#define MAXARG_sJ	((1 << SIZE_sJ) - 1)
#else
#define MAXARG_sJ	MAX_INT
#endif

// lua指令中sJ，不是采用二进制补码的方式进行编码，它采用了一个basis值作为0值，这个basis值是( ((1<<25)-1)>>1 )也就是16777215。
// 如果sJ表示-1时，那么它的值则是-1 + 16777215 = 16777214 = 0xFFFFFE，也就是说0表示sJ的最小值-16777214，0x1FFFFFF表示sBx的最大值33554431。
#define OFFSET_sJ	(MAXARG_sJ >> 1)


// A位能放入最大数值
#define MAXARG_A	((1<<SIZE_A)-1)
// B位能放入最大数值
#define MAXARG_B	((1<<SIZE_B)-1)
// C位能放入最大数值
#define MAXARG_C	((1<<SIZE_C)-1)
// 有符号C能放入最大数值
#define OFFSET_sC	(MAXARG_C >> 1)

// int转sC
#define int2sC(i)	((i) + OFFSET_sC)
// sC转int
#define sC2int(i)	((i) - OFFSET_sC)


/* creates a mask with 'n' 1 bits at position 'p' */
// 生成一个从第p位开始的n位掩码，掩码的值为1
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* creates a mask with 'n' 0 bits at position 'p' */
// 生成一个从第p位开始的n位掩码，掩码的值为0
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/

// 获取指令的操作码
#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
// 设置指令的操作码
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(Instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))

// 检测指令i是否等于指令模式m
#define checkopm(i,m)	(getOpMode(GET_OPCODE(i)) == m)


// 获取指定位置的参数
#define getarg(i,pos,size)	(cast_int(((i)>>(pos)) & MASK1(size,0)))
// 设置指定位置的参数
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                ((cast(Instruction, v)<<pos)&MASK1(size,pos))))

// 获取A参数的值
#define GETARG_A(i)	getarg(i, POS_A, SIZE_A)
// 设置A参数的值
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)

// 检测指令是否是iABC指令，并获取B参数的值
#define GETARG_B(i)	check_exp(checkopm(i, iABC), getarg(i, POS_B, SIZE_B))
#define GETARG_sB(i)	sC2int(GETARG_B(i))
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)

// 检测指令是否是iABC指令, 并获取C参数的值
#define GETARG_C(i)	check_exp(checkopm(i, iABC), getarg(i, POS_C, SIZE_C))
#define GETARG_sC(i)	sC2int(GETARG_C(i))
#define SETARG_C(i,v)	setarg(i, v, POS_C, SIZE_C)

#define TESTARG_k(i)	check_exp(checkopm(i, iABC), (cast_int(((i) & (1u << POS_k)))))
#define GETARG_k(i)	check_exp(checkopm(i, iABC), getarg(i, POS_k, 1))
#define SETARG_k(i,v)	setarg(i, v, POS_k, 1)

#define GETARG_Bx(i)	check_exp(checkopm(i, iABx), getarg(i, POS_Bx, SIZE_Bx))
#define SETARG_Bx(i,v)	setarg(i, v, POS_Bx, SIZE_Bx)

#define GETARG_Ax(i)	check_exp(checkopm(i, iAx), getarg(i, POS_Ax, SIZE_Ax))
#define SETARG_Ax(i,v)	setarg(i, v, POS_Ax, SIZE_Ax)

#define GETARG_sBx(i)  \
	check_exp(checkopm(i, iAsBx), getarg(i, POS_Bx, SIZE_Bx) - OFFSET_sBx)
#define SETARG_sBx(i,b)	SETARG_Bx((i),cast_uint((b)+OFFSET_sBx))

#define GETARG_sJ(i)  \
	check_exp(checkopm(i, isJ), getarg(i, POS_sJ, SIZE_sJ) - OFFSET_sJ)
#define SETARG_sJ(i,j) \
	setarg(i, cast_uint((j)+OFFSET_sJ), POS_sJ, SIZE_sJ)


// 创建iAbc指令
#define CREATE_ABCk(o,a,b,c,k)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, b)<<POS_B) \
			| (cast(Instruction, c)<<POS_C) \
			| (cast(Instruction, k)<<POS_k))

#define CREATE_ABx(o,a,bc)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, bc)<<POS_Bx))

// 创建iAx指令
#define CREATE_Ax(o,a)		((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_Ax))

// 创建isJ指令
#define CREATE_sJ(o,j,k)	((cast(Instruction, o) << POS_OP) \
			| (cast(Instruction, j) << POS_sJ) \
			| (cast(Instruction, k) << POS_k))


#if !defined(MAXINDEXRK)  /* (for debugging only) */
#define MAXINDEXRK	MAXARG_B
#endif


/*
** invalid register that fits in 8 bits
*/
// 不需要使用寄存器
#define NO_REG		MAXARG_A


/*
** R[x] - register
** K[x] - constant (in constant table)
** RK(x) == if k(i) then K[x] else R[x]
*/


/*
** Grep "ORDER OP" if you change these enums. Opcodes marked with a (*)
** has extra descriptions in the notes after the enumeration.
*/

typedef enum {
/*----------------------------------------------------------------------
  name		args	description
------------------------------------------------------------------------*/
// iABC，把一个寄存器的数据赋值到另一个寄存器；
OP_MOVE,/*	A B	R[A] := R[B]					*/
// iAsBx，加载一个[-65536,65535]范围内的整数到寄存器中
OP_LOADI,/*	A sBx	R[A] := sBx					*/
// iAsBx，加载一个[-65536,65535]范围内的整数到寄存器中，并强制转化为浮点数类型进行返回
OP_LOADF,/*	A sBx	R[A] := (lua_Number)sBx				*/
// iABx，以Bx为下标，读取常量数组的数据，然后把它存到以A为下标的寄存器中
OP_LOADK,/*	A Bx	R[A] := K[Bx]					*/
// iAx，上面OP_LOADK的B参数为17位二进制数，范围为[0,131071]，若一个Proto中声明的常量超过了这个数量
// （常出现在一些数据处理分析函数中，声明了很多不一样的数字），则无法使用OP_LOADK进行处理，
// 此时就需要使用OP_LOADKX指令。该指令从下一条指令Instruction的A寄存器中获取K数组的下标，
// 通常下一条指令的指令模式为iAx，它的A有25位，范围为[0,33555531]，提供了远比[0,131071]要大的数值范围。
OP_LOADKX,/*	A	R[A] := K[extra arg]				*/
// iABC，寄存器赋值为false；
OP_LOADFALSE,/*	A	R[A] := false					*/
// iABC，寄存器赋值为false；并让pc指针加1跳过下一条指令；
OP_LFALSESKIP,/*A	R[A] := false; pc++	(*)			*/
// iABC，寄存器赋值为true
OP_LOADTRUE,/*	A	R[A] := true					*/
// iABC，从R(A)到R(A+B)的值，全部赋值为NIL
OP_LOADNIL,/*	A B	R[A], R[A+1], ..., R[A+B] := nil		*/
// iABC，从upvals数组中读取下标为B的UpValue数据，并存入到下标为A的寄存器中
OP_GETUPVAL,/*	A B	R[A] := UpValue[B]				*/
// iABC，把下标为A的寄存器的数据，写入到upvals数组中下标为B元素中
OP_SETUPVAL,/*	A B	UpValue[B] := R[A]				*/

// iABC，首先从upvals数组中读取下标为B的UpValue数据，该数据需要为Table类型，
// 然后读取下标为C的常量数组k里面的数据，该数据需要为string类型，以该string类型数据作为Table的key值，
// 查询出Value值，并存入到下标为A的寄存器中
OP_GETTABUP,/*	A B C	R[A] := UpValue[B][K[C]:shortstring]		*/
// iABC，查询Table中key为元素的value值；
OP_GETTABLE,/*	A B C	R[A] := R[B][R[C]]				*/
// iABC，查询Table中key为特定整数的元素的value值；
OP_GETI,/*	A B C	R[A] := R[B][C]					*/
// iABC，查询Table中key为特定字符串的元素的value值；
OP_GETFIELD,/*	A B C	R[A] := R[B][K[C]:shortstring]			*/

// iABC，首先从upvals数组中读取下标为A的UpValue数据，该数据需要为Table类型，
// 然后读取下标为B的常量数组k里面的数据，该数据需要为string类型，以该string类型数据作为Table的key值，
// 查询出value的引用。最后根据k的值，以C为下标，从常量数组或者寄存器中读取数据，并赋值给该value。
OP_SETTABUP,/*	A B C	UpValue[A][K[B]:shortstring] := RK(C)		*/
// 修改Table中key的对象的value值；
OP_SETTABLE,/*	A B C	R[A][R[B]] := RK(C)				*/
// 修改Table中key为特定整数的对象的value值；
OP_SETI,/*	A B C	R[A][B] := RK(C)				*/
// 修改Table中key为特定字符串的对象的value值；
OP_SETFIELD,/*	A B C	R[A][K[B]:shortstring] := RK(C)			*/

// iABC，创建一个Table类型对象。参数B决定哈希表部分的初始大小，参数C和参数k决定了数组部分的初始大小。
OP_NEWTABLE,/*	A B C k	R[A] := {}					*/

OP_SELF,/*	A B C	R[A+1] := R[B]; R[A] := R[B][RK(C):string]	*/

// iABC，把一个寄存器与一个有符号整数相加，把结果存到另一个寄存器中。
OP_ADDI,/*	A B sC	R[A] := R[B] + sC				*/

// iABC，加法。寄存器与常量k数组中的一个常量相加，把结果存到另一个寄存器中；
OP_ADDK,/*	A B C	R[A] := R[B] + K[C]:number			*/
// 减法
OP_SUBK,/*	A B C	R[A] := R[B] - K[C]:number			*/
// 乘法
OP_MULK,/*	A B C	R[A] := R[B] * K[C]:number			*/
// 取模
OP_MODK,/*	A B C	R[A] := R[B] % K[C]:number			*/
// 幂
OP_POWK,/*	A B C	R[A] := R[B] ^ K[C]:number			*/
// 除法
OP_DIVK,/*	A B C	R[A] := R[B] / K[C]:number			*/
// 整除
OP_IDIVK,/*	A B C	R[A] := R[B] // K[C]:number			*/

// iABC，位与运算。一个寄存器与一个常量k数组中的数字进行按位与运算，把运算结果存到另一个寄存器中。
OP_BANDK,/*	A B C	R[A] := R[B] & K[C]:integer			*/
// 位或运算；
OP_BORK,/*	A B C	R[A] := R[B] | K[C]:integer			*/
// 位异或运算；
OP_BXORK,/*	A B C	R[A] := R[B] ~ K[C]:integer			*/

// iABC，右位移运算。一个寄存器二进制右位移一个数字常量的位数，把运算结果存到另一个寄存器中。
OP_SHRI,/*	A B sC	R[A] := R[B] >> sC				*/
// 左位移运算
OP_SHLI,/*	A B sC	R[A] := sC << R[B]				*/

// iABC，把两个寄存器的值相加，把结果存到另一个寄存器中。
// 加法
OP_ADD,/*	A B C	R[A] := R[B] + R[C]				*/
// 减法
OP_SUB,/*	A B C	R[A] := R[B] - R[C]				*/
// 乘法
OP_MUL,/*	A B C	R[A] := R[B] * R[C]				*/
// 取模
OP_MOD,/*	A B C	R[A] := R[B] % R[C]				*/
// 幂
OP_POW,/*	A B C	R[A] := R[B] ^ R[C]				*/
// 除法
OP_DIV,/*	A B C	R[A] := R[B] / R[C]				*/
// 整除
OP_IDIV,/*	A B C	R[A] := R[B] // R[C]				*/

// iABC，位与运算。2个寄存器做位与运算，把结果存到第3个寄存器中；
OP_BAND,/*	A B C	R[A] := R[B] & R[C]				*/
// 位或运算
OP_BOR,/*	A B C	R[A] := R[B] | R[C]				*/
// 位异或运算
OP_BXOR,/*	A B C	R[A] := R[B] ~ R[C]				*/
// 左位移
OP_SHL,/*	A B C	R[A] := R[B] << R[C]				*/
// 右位移
OP_SHR,/*	A B C	R[A] := R[B] >> R[C]				*/

// iABC，在Lua中，元方法支持最多两个操作数，左操作数为寄存器，右操作数根据类型的不同，可以把元方法相关的OpCode分成了以下3类：
// 右操作数也为寄存器，数据存储在运行的栈上；
OP_MMBIN,/*	A B C	call C metamethod over R[A] and R[B]	(*)	*/
// 右操作数为直接整数，在指令中直接存储数值；
OP_MMBINI,/*	A sB C k	call C metamethod over R[A] and sB	*/
// 右操作数为常量，存储在Proto的常量k数组中；
OP_MMBINK,/*	A B C k		call C metamethod over R[A] and K[B]	*/

// iABC，数值取负。
OP_UNM,/*	A B	R[A] := -R[B]					*/
// 位取反。
OP_BNOT,/*	A B	R[A] := ~R[B]					*/
// 条件取反。
OP_NOT,/*	A B	R[A] := not R[B]				*/
// 求对象的长度。常用于数组与字符串；
OP_LEN,/*	A B	R[A] := #R[B] (length operator)			*/

// iABC，字符串拼接。对应于".."符号拼接字符串；
OP_CONCAT,/*	A B	R[A] := R[A].. ... ..R[A + B - 1]		*/

OP_CLOSE,/*	A	close all upvalues >= R[A]			*/
OP_TBC,/*	A	mark variable A "to be closed"			*/
// isJ，J是有正负的，代表可以往前跳转或者往后跳转。
OP_JMP,/*	sJ	pc += sJ					*/
// iABC，根据指令中参数k的数值，判断两个寄存器的数据是否满足相等或者不等，若满足，则跳过下一条指令（通常下一条指令为跳转指令）
OP_EQ,/*	A B k	if ((R[A] == R[B]) ~= k) then pc++		*/
// 判断操作数的小于关系；
OP_LT,/*	A B k	if ((R[A] <  R[B]) ~= k) then pc++		*/
// 判断操作数的小于等于关系；
OP_LE,/*	A B k	if ((R[A] <= R[B]) ~= k) then pc++		*/
// 上述几条OpCode，都用到了指令中的参数k，k通常为0，代表采用正逻辑进行判断，例如OP_LT正逻辑就是小于；若k为1时，
// 代表采用反逻辑进行判断，OP_LT反逻辑对应的就是大于等于，由此实现了一条OpCode的多用。

// iABC，根据指令中参数k的数值，判断一个寄存器是否与常量k数组中的一个常量满足相等或者不等，若满足，则跳过下一条指令；
OP_EQK,/*	A B k	if ((R[A] == K[B]) ~= k) then pc++		*/
// iABC，根据指令中参数k的数值，判断一个寄存器是否与一个（有符号）数字满足相等或者不等，若满足，则跳过下一条指令；
OP_EQI,/*	A sB k	if ((R[A] == sB) ~= k) then pc++		*/
// 判断操作数的小于关系；
OP_LTI,/*	A sB k	if ((R[A] < sB) ~= k) then pc++			*/
// 判断操作数的小于等于关系；
OP_LEI,/*	A sB k	if ((R[A] <= sB) ~= k) then pc++		*/
// 判断操作数的大于关系；
OP_GTI,/*	A sB k	if ((R[A] > sB) ~= k) then pc++			*/
// 判断操作数的大于等于关系；
OP_GEI,/*	A sB k	if ((R[A] >= sB) ~= k) then pc++		*/

// iABC，根据指令参数k的值，判断一个寄存器是否为真或假，若满足，则跳过下一条指令。
OP_TEST,/*	A k	if (not R[A] == k) then pc++			*/
// iABC，根据指令参数k的值，判断一个寄存器是否为真或假，若满足，则跳过下一条指令；否则把该寄存器的值赋值给另一个寄存器。
// 与OP_TEST不同的是，OP_TESTSET用于判断与赋值同时存在的表达式中
OP_TESTSET,/*	A B k	if (not R[B] == k) then pc++ else R[A] := R[B] (*) */

// iABC，普通函数调用，含义简单来说就是对A寄存器中存储的函数进行调用，参数B决定参数数量，参数C决定返回值数量。
OP_CALL,/*	A B C	R[A], ... ,R[A+C-2] := R[A](R[A+1], ... ,R[A+B-1]) */
// 函数尾调用。与OP_CALL不同，OP_TAILCALL在调用函数的时候，不会插入新的堆栈信息，而是会复用当前的栈帧，
// 在存在大量函数递归调用的一些程序实现中，用尾调用的方式可避免函数调用堆栈溢出。
// 不过函数调用要触发尾调用，需要先满足苛刻的条件，需要该函数最后一句为以下格式：return XXX(params...)。
// 其中XXX即为要触发尾调用的函数。
OP_TAILCALL,/*	A B C k	return R[A](R[A+1], ... ,R[A+B-1])		*/

// iABC，可以支持非常多数量的返回值。
OP_RETURN,/*	A B C k	return R[A], ... ,R[A+B-2]	(see note)	*/
// 函数没有返回值；
OP_RETURN0,/*		return						*/
// 函数只有一个返回值；
OP_RETURN1,/*	A	return R[A]					*/

// iABX，循环体逻辑执行。有序执行循环内的其它OpCode，并在最后让循环次数减1，然后重新跳转回循环开始的，判断是否需要进入下一轮的循环执行。
OP_FORLOOP,/*	A Bx	update counters; if loop continues then pc-=Bx; */
// iABX，循环信息准备。在调用此OpCode前，堆栈上面需要先提前按顺序放置好循环控制变量、终点值，步长。
// 调用OP_FORPREP后，会根据堆栈上面的这些信息，为接下来的循环设置好计算环境，特别是提前计算出需要循环的次数，
// 判断是否是次数有效的非无限死循环。
OP_FORPREP,/*	A Bx	<check values and prepare counters>;
                        if not to run then pc+=Bx+1;			*/

// 泛型形式的循环比起数值形式要复杂一点。泛型形式的循环它需要一个迭代函数，
// 循环的结束次数也无法在循环开始前就通过数值运算计算出来，而是当迭代函数不再有返回值，
// 才代表循环已经结束。
// iABX，循环信息准备；
OP_TFORPREP,/*	A Bx	create upvalue for R[A + 3]; pc+=Bx		*/
// iABC，循环迭代函数执行；根据预先提供在栈上了的迭代函数，计算出本轮循环的变量。
OP_TFORCALL,/*	A C	R[A+4], ... ,R[A+3+C] := R[A](R[A+1], R[A+2]);	*/
// iABX，循环体逻辑执行。根据上述的OP_TFORCALL的返回，判断循环迭代函数是否正常返回了数据，若有，则跳转回循环主体部分有序执行循环内部逻辑的OpCode。
OP_TFORLOOP,/*	A Bx	if R[A+2] ~= nil then { R[A]=R[A+2]; pc -= Bx }	*/

// 以列表数据的形式的形式创建并初始化一个Table。
OP_SETLIST,/*	A B C k	R[A][C+i] := R[A+i], 1 <= i <= B		*/

// 根据函数原型创建闭包
OP_CLOSURE,/*	A Bx	R[A] := closure(KPROTO[Bx])			*/

// iABC，可变参数取值。表达式为R[A],R[A+1],...,R[A+C-2]=vararg，参数C表示期望获得的参数数量。
// 当我们在函数调用的参数列表中使用了可变参数，即函数定义时参数带有"..."，则在后续代码中每次使用"..."，
// 都会生成OP_VARARG，用于处理从可变参数中的取值。
OP_VARARG,/*	A C	R[A], R[A+1], ..., R[A+C-2] = vararg		*/

// iABC，所有函数在调用前需要先调用此OpCode完成堆栈准备，
// OP_VARARGPREP会把将要调用的函数以及调用时传入的参数依次拷贝到栈顶位置，便于后续的函数操作。
OP_VARARGPREP,/*A	(adjust vararg parameters)			*/

OP_EXTRAARG/*	Ax	extra (larger) argument for previous opcode	*/
} OpCode;


#define NUM_OPCODES	((int)(OP_EXTRAARG) + 1)



/*===========================================================================
  Notes:

  (*) Opcode OP_LFALSESKIP is used to convert a condition to a boolean
  value, in a code equivalent to (not cond ? false : true).  (It
  produces false and skips the next instruction producing true.)

  (*) Opcodes OP_MMBIN and variants follow each arithmetic and
  bitwise opcode. If the operation succeeds, it skips this next
  opcode. Otherwise, this opcode calls the corresponding metamethod.

  (*) Opcode OP_TESTSET is used in short-circuit expressions that need
  both to jump and to produce a value, such as (a = b or c).

  (*) In OP_CALL, if (B == 0) then B = top - A. If (C == 0), then
  'top' is set to last_result+1, so next open instruction (OP_CALL,
  OP_RETURN*, OP_SETLIST) may use 'top'.

  (*) In OP_VARARG, if (C == 0) then use actual number of varargs and
  set top (like in OP_CALL with C == 0).

  (*) In OP_RETURN, if (B == 0) then return up to 'top'.

  (*) In OP_LOADKX and OP_NEWTABLE, the next instruction is always
  OP_EXTRAARG.

  (*) In OP_SETLIST, if (B == 0) then real B = 'top'; if k, then
  real C = EXTRAARG _ C (the bits of EXTRAARG concatenated with the
  bits of C).

  (*) In OP_NEWTABLE, B is log2 of the hash size (which is always a
  power of 2) plus 1, or zero for size zero. If not k, the array size
  is C. Otherwise, the array size is EXTRAARG _ C.

  (*) For comparisons, k specifies what condition the test should accept
  (true or false).

  (*) In OP_MMBINI/OP_MMBINK, k means the arguments were flipped
   (the constant is the first operand).

  (*) All 'skips' (pc++) assume that next instruction is a jump.

  (*) In instructions OP_RETURN/OP_TAILCALL, 'k' specifies that the
  function builds upvalues, which may need to be closed. C > 0 means
  the function is vararg, so that its 'func' must be corrected before
  returning; in this case, (C - 1) is its number of fixed parameters.

  (*) In comparisons with an immediate operand, C signals whether the
  original operand was a float. (It must be corrected in case of
  metamethods.)

===========================================================================*/


/*
** masks for instruction properties. The format is:
** bits 0-2: op mode
** bit 3: instruction set register A
** bit 4: operator is a test (next instruction must be a jump)
** bit 5: instruction uses 'L->top' set by previous instruction (when B == 0)
** bit 6: instruction sets 'L->top' for next instruction (when C == 0)
** bit 7: instruction is an MM instruction (call a metamethod)
*/
/*
** 指令属性的掩码
** 位0-2：指令的类型{iABC, iABx, iAsBx, iAx, isJ}
** 位3：是否有这个指令赋值给R(A)
** 位4：是否是条件指令，下一个指令肯定是跳转
** 位5：是否使用前一条指令设置的栈顶值（当 B == 0 时） 
** 位6：是否为下条指令设置 L->top（当操作数C为0时）
** 位7：指令是MM指令（调用元方法）
*/

LUAI_DDEC(const lu_byte luaP_opmodes[NUM_OPCODES];)

// 获取指令的类型
#define getOpMode(m)	(cast(enum OpMode, luaP_opmodes[m] & 7))
// 检查指令会给A寄存器赋值
#define testAMode(m)	(luaP_opmodes[m] & (1 << 3))
// 检查是否是条件指令，下一个指令肯定是跳转
#define testTMode(m)	(luaP_opmodes[m] & (1 << 4))
// 检查是否使用前一条指令设置的栈顶值（当 B == 0 时）
#define testITMode(m)	(luaP_opmodes[m] & (1 << 5))
// 检查是否为下条指令设置 L->top（当操作数C为0时）
#define testOTMode(m)	(luaP_opmodes[m] & (1 << 6))
// 检查指令是MM指令（调用元方法）
#define testMMMode(m)	(luaP_opmodes[m] & (1 << 7))

/* "out top" (set top for next instruction) */
#define isOT(i)  \
	((testOTMode(GET_OPCODE(i)) && GETARG_C(i) == 0) || \
          GET_OPCODE(i) == OP_TAILCALL)

/* "in top" (uses top from previous instruction) */
#define isIT(i)		(testITMode(GET_OPCODE(i)) && GETARG_B(i) == 0)

// mm:指令是MM指令（调用元方法）
// ot:是否为下条指令设置 L->top（当操作数C为0时）
// it:是否使用前一条指令设置的栈顶值（当 B == 0 时） 
// t:是否是条件指令，下一个指令肯定是跳转
// a:是否有这个指令赋值给R(A)
// m:指令的类型{iABC, iABx, iAsBx, iAx, isJ}
#define opmode(mm,ot,it,t,a,m)  \
    (((mm) << 7) | ((ot) << 6) | ((it) << 5) | ((t) << 4) | ((a) << 3) | (m))


/* number of list items to accumulate before a SETLIST instruction */
#define LFIELDS_PER_FLUSH	50

#endif
