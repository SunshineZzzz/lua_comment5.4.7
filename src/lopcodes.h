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


// ָ��ģʽ
// iABC�������ޣ�4����8λ��A��2��8�η�������ܴ洢256��������8λ��B��8λ��C��1λ��2��1�η�������ܴ洢2��������k
// iABC����õ�ָ��ģʽ
// 
// iABx�������ޣ�2����8λ��A��17λ�޷��ŵ�Bx��Bx��2��17�η�������ܴ洢131072������
// 
// iAsBx�������ޣ�2����8λ��A��17λ�з��ŵ�Bx��sBx��2��17�η�������ܴ洢131072������
// 
// iAx�������ޣ�1����25λ�޷��ŵ�Ax��Ax��2��25�η�������ܴ洢33554432������
// 
// isJ�������ޣ�1����25λ�з��ŵ�sJ��sJ��2��25�η�������ܴ洢33554432������
// Ŀǰ��Ψһʹ�ø�ָ��ģʽ��ֻ����תָ��OP_JMP�����Ը�ָ��ģʽ����'isJ'������'sJ'��������ĸ��J��(Jump����ת)��
// ����Ϊ�˸���ֱ�۵�֪�����ָ��ģʽ��Ӧ������ת����
enum OpMode {iABC, iABx, iAsBx, iAx, isJ};  /* basic instruction formats */


/*
** size and position of opcode arguments.
*/
// C������С8λ
#define SIZE_C		8
// B������С8λ
#define SIZE_B		8
// Bx17λ
#define SIZE_Bx		(SIZE_C + SIZE_B + 1)
// A������С8λ
#define SIZE_A		8
// Ax������С25λ
#define SIZE_Ax		(SIZE_Bx + SIZE_A)
// sJ������С25λ
#define SIZE_sJ		(SIZE_Bx + SIZE_A)

// ָ���
#define SIZE_OP		7

// ָ����ʼλ��
#define POS_OP		0

// A������ʼλ�ã�7
#define POS_A		(POS_OP + SIZE_OP)
// k������ʼλ�ã�15
#define POS_k		(POS_A + SIZE_A)
// B������ʼλ�ã�16
#define POS_B		(POS_k + 1)
// C������ʼλ�ã�24
#define POS_C		(POS_B + SIZE_B)

// Bx������ʼλ�ã�15
#define POS_Bx		POS_k

// Ax������ʼλ�ã�7
#define POS_Ax		POS_A

// sJ������ʼλ�ã�7
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

// luaָ����sJ�����ǲ��ö����Ʋ���ķ�ʽ���б��룬��������һ��basisֵ��Ϊ0ֵ�����basisֵ��( ((1<<25)-1)>>1 )Ҳ����16777215��
// ���sJ��ʾ-1ʱ����ô����ֵ����-1 + 16777215 = 16777214 = 0xFFFFFE��Ҳ����˵0��ʾsJ����Сֵ-16777214��0x1FFFFFF��ʾsBx�����ֵ33554431��
#define OFFSET_sJ	(MAXARG_sJ >> 1)


// Aλ�ܷ��������ֵ
#define MAXARG_A	((1<<SIZE_A)-1)
// Bλ�ܷ��������ֵ
#define MAXARG_B	((1<<SIZE_B)-1)
// Cλ�ܷ��������ֵ
#define MAXARG_C	((1<<SIZE_C)-1)
// �з���C�ܷ��������ֵ
#define OFFSET_sC	(MAXARG_C >> 1)

// intתsC
#define int2sC(i)	((i) + OFFSET_sC)
// sCתint
#define sC2int(i)	((i) - OFFSET_sC)


/* creates a mask with 'n' 1 bits at position 'p' */
// ����һ���ӵ�pλ��ʼ��nλ���룬�����ֵΪ1
#define MASK1(n,p)	((~((~(Instruction)0)<<(n)))<<(p))

/* creates a mask with 'n' 0 bits at position 'p' */
// ����һ���ӵ�pλ��ʼ��nλ���룬�����ֵΪ0
#define MASK0(n,p)	(~MASK1(n,p))

/*
** the following macros help to manipulate instructions
*/

// ��ȡָ��Ĳ�����
#define GET_OPCODE(i)	(cast(OpCode, ((i)>>POS_OP) & MASK1(SIZE_OP,0)))
// ����ָ��Ĳ�����
#define SET_OPCODE(i,o)	((i) = (((i)&MASK0(SIZE_OP,POS_OP)) | \
		((cast(Instruction, o)<<POS_OP)&MASK1(SIZE_OP,POS_OP))))

// ���ָ��i�Ƿ����ָ��ģʽm
#define checkopm(i,m)	(getOpMode(GET_OPCODE(i)) == m)


// ��ȡָ��λ�õĲ���
#define getarg(i,pos,size)	(cast_int(((i)>>(pos)) & MASK1(size,0)))
// ����ָ��λ�õĲ���
#define setarg(i,v,pos,size)	((i) = (((i)&MASK0(size,pos)) | \
                ((cast(Instruction, v)<<pos)&MASK1(size,pos))))

// ��ȡA������ֵ
#define GETARG_A(i)	getarg(i, POS_A, SIZE_A)
// ����A������ֵ
#define SETARG_A(i,v)	setarg(i, v, POS_A, SIZE_A)

// ���ָ���Ƿ���iABCָ�����ȡB������ֵ
#define GETARG_B(i)	check_exp(checkopm(i, iABC), getarg(i, POS_B, SIZE_B))
#define GETARG_sB(i)	sC2int(GETARG_B(i))
#define SETARG_B(i,v)	setarg(i, v, POS_B, SIZE_B)

// ���ָ���Ƿ���iABCָ��, ����ȡC������ֵ
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


// ����iAbcָ��
#define CREATE_ABCk(o,a,b,c,k)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, b)<<POS_B) \
			| (cast(Instruction, c)<<POS_C) \
			| (cast(Instruction, k)<<POS_k))

#define CREATE_ABx(o,a,bc)	((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_A) \
			| (cast(Instruction, bc)<<POS_Bx))

// ����iAxָ��
#define CREATE_Ax(o,a)		((cast(Instruction, o)<<POS_OP) \
			| (cast(Instruction, a)<<POS_Ax))

// ����isJָ��
#define CREATE_sJ(o,j,k)	((cast(Instruction, o) << POS_OP) \
			| (cast(Instruction, j) << POS_sJ) \
			| (cast(Instruction, k) << POS_k))


#if !defined(MAXINDEXRK)  /* (for debugging only) */
#define MAXINDEXRK	MAXARG_B
#endif


/*
** invalid register that fits in 8 bits
*/
// ����Ҫʹ�üĴ���
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
// iABC����һ���Ĵ��������ݸ�ֵ����һ���Ĵ�����
OP_MOVE,/*	A B	R[A] := R[B]					*/
// iAsBx������һ��[-65536,65535]��Χ�ڵ��������Ĵ�����
OP_LOADI,/*	A sBx	R[A] := sBx					*/
// iAsBx������һ��[-65536,65535]��Χ�ڵ��������Ĵ����У���ǿ��ת��Ϊ���������ͽ��з���
OP_LOADF,/*	A sBx	R[A] := (lua_Number)sBx				*/
// iABx����BxΪ�±꣬��ȡ������������ݣ�Ȼ������浽��AΪ�±�ļĴ�����
OP_LOADK,/*	A Bx	R[A] := K[Bx]					*/
// iAx������OP_LOADK��B����Ϊ17λ������������ΧΪ[0,131071]����һ��Proto�������ĳ����������������
// ����������һЩ���ݴ�����������У������˺ܶ಻һ�������֣������޷�ʹ��OP_LOADK���д���
// ��ʱ����Ҫʹ��OP_LOADKXָ���ָ�����һ��ָ��Instruction��A�Ĵ����л�ȡK������±꣬
// ͨ����һ��ָ���ָ��ģʽΪiAx������A��25λ����ΧΪ[0,33555531]���ṩ��Զ��[0,131071]Ҫ�����ֵ��Χ��
OP_LOADKX,/*	A	R[A] := K[extra arg]				*/
// iABC���Ĵ�����ֵΪfalse��
OP_LOADFALSE,/*	A	R[A] := false					*/
// iABC���Ĵ�����ֵΪfalse������pcָ���1������һ��ָ�
OP_LFALSESKIP,/*A	R[A] := false; pc++	(*)			*/
// iABC���Ĵ�����ֵΪtrue
OP_LOADTRUE,/*	A	R[A] := true					*/
// iABC����R(A)��R(A+B)��ֵ��ȫ����ֵΪNIL
OP_LOADNIL,/*	A B	R[A], R[A+1], ..., R[A+B] := nil		*/
// iABC����upvals�����ж�ȡ�±�ΪB��UpValue���ݣ������뵽�±�ΪA�ļĴ�����
OP_GETUPVAL,/*	A B	R[A] := UpValue[B]				*/
// iABC�����±�ΪA�ļĴ��������ݣ�д�뵽upvals�������±�ΪBԪ����
OP_SETUPVAL,/*	A B	UpValue[B] := R[A]				*/

// iABC�����ȴ�upvals�����ж�ȡ�±�ΪB��UpValue���ݣ���������ҪΪTable���ͣ�
// Ȼ���ȡ�±�ΪC�ĳ�������k��������ݣ���������ҪΪstring���ͣ��Ը�string����������ΪTable��keyֵ��
// ��ѯ��Valueֵ�������뵽�±�ΪA�ļĴ�����
OP_GETTABUP,/*	A B C	R[A] := UpValue[B][K[C]:shortstring]		*/
// iABC����ѯTable��keyΪԪ�ص�valueֵ��
OP_GETTABLE,/*	A B C	R[A] := R[B][R[C]]				*/
// iABC����ѯTable��keyΪ�ض�������Ԫ�ص�valueֵ��
OP_GETI,/*	A B C	R[A] := R[B][C]					*/
// iABC����ѯTable��keyΪ�ض��ַ�����Ԫ�ص�valueֵ��
OP_GETFIELD,/*	A B C	R[A] := R[B][K[C]:shortstring]			*/

// iABC�����ȴ�upvals�����ж�ȡ�±�ΪA��UpValue���ݣ���������ҪΪTable���ͣ�
// Ȼ���ȡ�±�ΪB�ĳ�������k��������ݣ���������ҪΪstring���ͣ��Ը�string����������ΪTable��keyֵ��
// ��ѯ��value�����á�������k��ֵ����CΪ�±꣬�ӳ���������߼Ĵ����ж�ȡ���ݣ�����ֵ����value��
OP_SETTABUP,/*	A B C	UpValue[A][K[B]:shortstring] := RK(C)		*/
// �޸�Table��key�Ķ����valueֵ��
OP_SETTABLE,/*	A B C	R[A][R[B]] := RK(C)				*/
// �޸�Table��keyΪ�ض������Ķ����valueֵ��
OP_SETI,/*	A B C	R[A][B] := RK(C)				*/
// �޸�Table��keyΪ�ض��ַ����Ķ����valueֵ��
OP_SETFIELD,/*	A B C	R[A][K[B]:shortstring] := RK(C)			*/

// iABC������һ��Table���Ͷ��󡣲���B������ϣ���ֵĳ�ʼ��С������C�Ͳ���k���������鲿�ֵĳ�ʼ��С��
OP_NEWTABLE,/*	A B C k	R[A] := {}					*/

OP_SELF,/*	A B C	R[A+1] := R[B]; R[A] := R[B][RK(C):string]	*/

// iABC����һ���Ĵ�����һ���з���������ӣ��ѽ���浽��һ���Ĵ����С�
OP_ADDI,/*	A B sC	R[A] := R[B] + sC				*/

// iABC���ӷ����Ĵ����볣��k�����е�һ��������ӣ��ѽ���浽��һ���Ĵ����У�
OP_ADDK,/*	A B C	R[A] := R[B] + K[C]:number			*/
// ����
OP_SUBK,/*	A B C	R[A] := R[B] - K[C]:number			*/
// �˷�
OP_MULK,/*	A B C	R[A] := R[B] * K[C]:number			*/
// ȡģ
OP_MODK,/*	A B C	R[A] := R[B] % K[C]:number			*/
// ��
OP_POWK,/*	A B C	R[A] := R[B] ^ K[C]:number			*/
// ����
OP_DIVK,/*	A B C	R[A] := R[B] / K[C]:number			*/
// ����
OP_IDIVK,/*	A B C	R[A] := R[B] // K[C]:number			*/

// iABC��λ�����㡣һ���Ĵ�����һ������k�����е����ֽ��а�λ�����㣬���������浽��һ���Ĵ����С�
OP_BANDK,/*	A B C	R[A] := R[B] & K[C]:integer			*/
// λ�����㣻
OP_BORK,/*	A B C	R[A] := R[B] | K[C]:integer			*/
// λ������㣻
OP_BXORK,/*	A B C	R[A] := R[B] ~ K[C]:integer			*/

// iABC����λ�����㡣һ���Ĵ�����������λ��һ�����ֳ�����λ�������������浽��һ���Ĵ����С�
OP_SHRI,/*	A B sC	R[A] := R[B] >> sC				*/
// ��λ������
OP_SHLI,/*	A B sC	R[A] := sC << R[B]				*/

// iABC���������Ĵ�����ֵ��ӣ��ѽ���浽��һ���Ĵ����С�
// �ӷ�
OP_ADD,/*	A B C	R[A] := R[B] + R[C]				*/
// ����
OP_SUB,/*	A B C	R[A] := R[B] - R[C]				*/
// �˷�
OP_MUL,/*	A B C	R[A] := R[B] * R[C]				*/
// ȡģ
OP_MOD,/*	A B C	R[A] := R[B] % R[C]				*/
// ��
OP_POW,/*	A B C	R[A] := R[B] ^ R[C]				*/
// ����
OP_DIV,/*	A B C	R[A] := R[B] / R[C]				*/
// ����
OP_IDIV,/*	A B C	R[A] := R[B] // R[C]				*/

// iABC��λ�����㡣2���Ĵ�����λ�����㣬�ѽ���浽��3���Ĵ����У�
OP_BAND,/*	A B C	R[A] := R[B] & R[C]				*/
// λ������
OP_BOR,/*	A B C	R[A] := R[B] | R[C]				*/
// λ�������
OP_BXOR,/*	A B C	R[A] := R[B] ~ R[C]				*/
// ��λ��
OP_SHL,/*	A B C	R[A] := R[B] << R[C]				*/
// ��λ��
OP_SHR,/*	A B C	R[A] := R[B] >> R[C]				*/

// iABC����Lua�У�Ԫ����֧������������������������Ϊ�Ĵ������Ҳ������������͵Ĳ�ͬ�����԰�Ԫ������ص�OpCode�ֳ�������3�ࣺ
// �Ҳ�����ҲΪ�Ĵ��������ݴ洢�����е�ջ�ϣ�
OP_MMBIN,/*	A B C	call C metamethod over R[A] and R[B]	(*)	*/
// �Ҳ�����Ϊֱ����������ָ����ֱ�Ӵ洢��ֵ��
OP_MMBINI,/*	A sB C k	call C metamethod over R[A] and sB	*/
// �Ҳ�����Ϊ�������洢��Proto�ĳ���k�����У�
OP_MMBINK,/*	A B C k		call C metamethod over R[A] and K[B]	*/

// iABC����ֵȡ����
OP_UNM,/*	A B	R[A] := -R[B]					*/
// λȡ����
OP_BNOT,/*	A B	R[A] := ~R[B]					*/
// ����ȡ����
OP_NOT,/*	A B	R[A] := not R[B]				*/
// �����ĳ��ȡ��������������ַ�����
OP_LEN,/*	A B	R[A] := #R[B] (length operator)			*/

// iABC���ַ���ƴ�ӡ���Ӧ��".."����ƴ���ַ�����
OP_CONCAT,/*	A B	R[A] := R[A].. ... ..R[A + B - 1]		*/

OP_CLOSE,/*	A	close all upvalues >= R[A]			*/
OP_TBC,/*	A	mark variable A "to be closed"			*/
// isJ��J���������ģ����������ǰ��ת����������ת��
OP_JMP,/*	sJ	pc += sJ					*/
// iABC������ָ���в���k����ֵ���ж������Ĵ����������Ƿ�������Ȼ��߲��ȣ������㣬��������һ��ָ�ͨ����һ��ָ��Ϊ��תָ�
OP_EQ,/*	A B k	if ((R[A] == R[B]) ~= k) then pc++		*/
// �жϲ�������С�ڹ�ϵ��
OP_LT,/*	A B k	if ((R[A] <  R[B]) ~= k) then pc++		*/
// �жϲ�������С�ڵ��ڹ�ϵ��
OP_LE,/*	A B k	if ((R[A] <= R[B]) ~= k) then pc++		*/
// ��������OpCode�����õ���ָ���еĲ���k��kͨ��Ϊ0������������߼������жϣ�����OP_LT���߼�����С�ڣ���kΪ1ʱ��
// ������÷��߼������жϣ�OP_LT���߼���Ӧ�ľ��Ǵ��ڵ��ڣ��ɴ�ʵ����һ��OpCode�Ķ��á�

// iABC������ָ���в���k����ֵ���ж�һ���Ĵ����Ƿ��볣��k�����е�һ������������Ȼ��߲��ȣ������㣬��������һ��ָ�
OP_EQK,/*	A B k	if ((R[A] == K[B]) ~= k) then pc++		*/
// iABC������ָ���в���k����ֵ���ж�һ���Ĵ����Ƿ���һ�����з��ţ�����������Ȼ��߲��ȣ������㣬��������һ��ָ�
OP_EQI,/*	A sB k	if ((R[A] == sB) ~= k) then pc++		*/
// �жϲ�������С�ڹ�ϵ��
OP_LTI,/*	A sB k	if ((R[A] < sB) ~= k) then pc++			*/
// �жϲ�������С�ڵ��ڹ�ϵ��
OP_LEI,/*	A sB k	if ((R[A] <= sB) ~= k) then pc++		*/
// �жϲ������Ĵ��ڹ�ϵ��
OP_GTI,/*	A sB k	if ((R[A] > sB) ~= k) then pc++			*/
// �жϲ������Ĵ��ڵ��ڹ�ϵ��
OP_GEI,/*	A sB k	if ((R[A] >= sB) ~= k) then pc++		*/

// iABC������ָ�����k��ֵ���ж�һ���Ĵ����Ƿ�Ϊ���٣������㣬��������һ��ָ�
OP_TEST,/*	A k	if (not R[A] == k) then pc++			*/
// iABC������ָ�����k��ֵ���ж�һ���Ĵ����Ƿ�Ϊ���٣������㣬��������һ��ָ�����ѸüĴ�����ֵ��ֵ����һ���Ĵ�����
// ��OP_TEST��ͬ���ǣ�OP_TESTSET�����ж��븳ֵͬʱ���ڵı��ʽ��
OP_TESTSET,/*	A B k	if (not R[B] == k) then pc++ else R[A] := R[B] (*) */

// iABC����ͨ�������ã��������˵���Ƕ�A�Ĵ����д洢�ĺ������е��ã�����B������������������C��������ֵ������
OP_CALL,/*	A B C	R[A], ... ,R[A+C-2] := R[A](R[A+1], ... ,R[A+B-1]) */
// ����β���á���OP_CALL��ͬ��OP_TAILCALL�ڵ��ú�����ʱ�򣬲�������µĶ�ջ��Ϣ�����ǻḴ�õ�ǰ��ջ֡��
// �ڴ��ڴ��������ݹ���õ�һЩ����ʵ���У���β���õķ�ʽ�ɱ��⺯�����ö�ջ�����
// ������������Ҫ����β���ã���Ҫ��������̵���������Ҫ�ú������һ��Ϊ���¸�ʽ��return XXX(params...)��
// ����XXX��ΪҪ����β���õĺ�����
OP_TAILCALL,/*	A B C k	return R[A](R[A+1], ... ,R[A+B-1])		*/

// iABC������֧�ַǳ��������ķ���ֵ��
OP_RETURN,/*	A B C k	return R[A], ... ,R[A+B-2]	(see note)	*/
// ����û�з���ֵ��
OP_RETURN0,/*		return						*/
// ����ֻ��һ������ֵ��
OP_RETURN1,/*	A	return R[A]					*/

// iABX��ѭ�����߼�ִ�С�����ִ��ѭ���ڵ�����OpCode�����������ѭ��������1��Ȼ��������ת��ѭ����ʼ�ģ��ж��Ƿ���Ҫ������һ�ֵ�ѭ��ִ�С�
OP_FORLOOP,/*	A Bx	update counters; if loop continues then pc-=Bx; */
// iABX��ѭ����Ϣ׼�����ڵ��ô�OpCodeǰ����ջ������Ҫ����ǰ��˳����ú�ѭ�����Ʊ������յ�ֵ��������
// ����OP_FORPREP�󣬻���ݶ�ջ�������Щ��Ϣ��Ϊ��������ѭ�����úü��㻷�����ر�����ǰ�������Ҫѭ���Ĵ�����
// �ж��Ƿ��Ǵ�����Ч�ķ�������ѭ����
OP_FORPREP,/*	A Bx	<check values and prepare counters>;
                        if not to run then pc+=Bx+1;			*/

// ������ʽ��ѭ��������ֵ��ʽҪ����һ�㡣������ʽ��ѭ������Ҫһ������������
// ѭ���Ľ�������Ҳ�޷���ѭ����ʼǰ��ͨ����ֵ���������������ǵ��������������з���ֵ��
// �Ŵ���ѭ���Ѿ�������
// iABX��ѭ����Ϣ׼����
OP_TFORPREP,/*	A Bx	create upvalue for R[A + 3]; pc+=Bx		*/
// iABC��ѭ����������ִ�У�����Ԥ���ṩ��ջ���˵ĵ������������������ѭ���ı�����
OP_TFORCALL,/*	A C	R[A+4], ... ,R[A+3+C] := R[A](R[A+1], R[A+2]);	*/
// iABX��ѭ�����߼�ִ�С�����������OP_TFORCALL�ķ��أ��ж�ѭ�����������Ƿ��������������ݣ����У�����ת��ѭ�����岿������ִ��ѭ���ڲ��߼���OpCode��
OP_TFORLOOP,/*	A Bx	if R[A+2] ~= nil then { R[A]=R[A+2]; pc -= Bx }	*/

// ���б����ݵ���ʽ����ʽ��������ʼ��һ��Table��
OP_SETLIST,/*	A B C k	R[A][C+i] := R[A+i], 1 <= i <= B		*/

// ���ݺ���ԭ�ʹ����հ�
OP_CLOSURE,/*	A Bx	R[A] := closure(KPROTO[Bx])			*/

// iABC���ɱ����ȡֵ�����ʽΪR[A],R[A+1],...,R[A+C-2]=vararg������C��ʾ������õĲ���������
// �������ں������õĲ����б���ʹ���˿ɱ����������������ʱ��������"..."�����ں���������ÿ��ʹ��"..."��
// ��������OP_VARARG�����ڴ���ӿɱ�����е�ȡֵ��
OP_VARARG,/*	A C	R[A], R[A+1], ..., R[A+C-2] = vararg		*/

// iABC�����к����ڵ���ǰ��Ҫ�ȵ��ô�OpCode��ɶ�ջ׼����
// OP_VARARGPREP��ѽ�Ҫ���õĺ����Լ�����ʱ����Ĳ������ο�����ջ��λ�ã����ں����ĺ���������
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
** ָ�����Ե�����
** λ0-2��ָ�������{iABC, iABx, iAsBx, iAx, isJ}
** λ3���Ƿ������ָ�ֵ��R(A)
** λ4���Ƿ�������ָ���һ��ָ��϶�����ת
** λ5���Ƿ�ʹ��ǰһ��ָ�����õ�ջ��ֵ���� B == 0 ʱ�� 
** λ6���Ƿ�Ϊ����ָ������ L->top����������CΪ0ʱ��
** λ7��ָ����MMָ�����Ԫ������
*/

LUAI_DDEC(const lu_byte luaP_opmodes[NUM_OPCODES];)

// ��ȡָ�������
#define getOpMode(m)	(cast(enum OpMode, luaP_opmodes[m] & 7))
// ���ָ����A�Ĵ�����ֵ
#define testAMode(m)	(luaP_opmodes[m] & (1 << 3))
// ����Ƿ�������ָ���һ��ָ��϶�����ת
#define testTMode(m)	(luaP_opmodes[m] & (1 << 4))
// ����Ƿ�ʹ��ǰһ��ָ�����õ�ջ��ֵ���� B == 0 ʱ��
#define testITMode(m)	(luaP_opmodes[m] & (1 << 5))
// ����Ƿ�Ϊ����ָ������ L->top����������CΪ0ʱ��
#define testOTMode(m)	(luaP_opmodes[m] & (1 << 6))
// ���ָ����MMָ�����Ԫ������
#define testMMMode(m)	(luaP_opmodes[m] & (1 << 7))

/* "out top" (set top for next instruction) */
#define isOT(i)  \
	((testOTMode(GET_OPCODE(i)) && GETARG_C(i) == 0) || \
          GET_OPCODE(i) == OP_TAILCALL)

/* "in top" (uses top from previous instruction) */
#define isIT(i)		(testITMode(GET_OPCODE(i)) && GETARG_B(i) == 0)

// mm:ָ����MMָ�����Ԫ������
// ot:�Ƿ�Ϊ����ָ������ L->top����������CΪ0ʱ��
// it:�Ƿ�ʹ��ǰһ��ָ�����õ�ջ��ֵ���� B == 0 ʱ�� 
// t:�Ƿ�������ָ���һ��ָ��϶�����ת
// a:�Ƿ������ָ�ֵ��R(A)
// m:ָ�������{iABC, iABx, iAsBx, iAx, isJ}
#define opmode(mm,ot,it,t,a,m)  \
    (((mm) << 7) | ((ot) << 6) | ((it) << 5) | ((t) << 4) | ((a) << 3) | (m))


/* number of list items to accumulate before a SETLIST instruction */
#define LFIELDS_PER_FLUSH	50

#endif
