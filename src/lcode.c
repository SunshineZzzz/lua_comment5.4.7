/*
** $Id: lcode.c $
** Code generator for Lua
** See Copyright Notice in lua.h
*/

#define lcode_c
#define LUA_CORE

#include "lprefix.h"


#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lgc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"


/* Maximum number of registers in a Lua function (must fit in 8 bits) */
#define MAXREGS		255


#define hasjumps(e)	((e)->t != (e)->f)


static int codesJ (FuncState *fs, OpCode o, int sj, int k);



/* semantic error */
l_noret luaK_semerror (LexState *ls, const char *msg) {
  ls->t.token = 0;  /* remove "near <token>" from final message */
  luaX_syntaxerror(ls, msg);
}


/*
** If expression is a numeric constant, fills 'v' with its value
** and returns 1. Otherwise, returns 0.
*/
// 检查表达式是否是一个数值常量（整数或浮点数），并在满足条件时将其值填充到目标变量中
// 返回值1表示确实是数值，返回值0表示不是
static int tonumeral (const expdesc *e, TValue *v) {
  if (hasjumps(e))
    return 0;  /* not a numeral */
  switch (e->k) {
    case VKINT:
      if (v) setivalue(v, e->u.ival);
      return 1;
    case VKFLT:
      if (v) setfltvalue(v, e->u.nval);
      return 1;
    default: return 0;
  }
}


/*
** Get the constant value from a constant expression
*/
// 从表达式描述信息中提取编译时常量变量（Compile-Time Constant, CTC）的值
static TValue *const2val (FuncState *fs, const expdesc *e) {
  lua_assert(e->k == VCONST);
  return &fs->ls->dyd->actvar.arr[e->u.info].k;
}


/*
** If expression is a constant, fills 'v' with its value
** and returns 1. Otherwise, returns 0.
*/
// 检查表达式e是否是常量，如果是，填充v并返回1，否则返回0
int luaK_exp2const (FuncState *fs, const expdesc *e, TValue *v) {
  if (hasjumps(e))
    return 0;  /* not a constant */
  switch (e->k) {
    case VFALSE:
      setbfvalue(v);
      return 1;
    case VTRUE:
      setbtvalue(v);
      return 1;
    case VNIL:
      setnilvalue(v);
      return 1;
    case VKSTR: {
      setsvalue(fs->ls->L, v, e->u.strval);
      return 1;
    }
    case VCONST: {
      // 前面已经编译优化好的
      setobj(fs->ls->L, v, const2val(fs, e));
      return 1;
    }
    // 数值常量
    default: return tonumeral(e, v);
  }
}


/*
** Return the previous instruction of the current code. If there
** may be a jump target between the current instruction and the
** previous one, return an invalid instruction (to avoid wrong
** optimizations).
*/
// 返回当前代码的前一条指令。如果当前指令与前一条指令之间可能存在跳转目标，
// 则返回无效指令（以避免错误的优化操作）。
static Instruction *previousinstruction (FuncState *fs) {
  // 无效指令
  static const Instruction invalidinstruction = ~(Instruction)0;
  // 如果fs->pc > fs->lasttarget，说明当前指令与前一条指令之间没有跳转目标，
  // 可以安全地返回前一条指令
  if (fs->pc > fs->lasttarget)
    return &fs->f->code[fs->pc - 1];  /* previous instruction */
  else
    return cast(Instruction*, &invalidinstruction);
}


/*
** Create a OP_LOADNIL instruction, but try to optimize: if the previous
** instruction is also OP_LOADNIL and ranges are compatible, adjust
** range of previous instruction instead of emitting a new one. (For
** instance, 'local a; local b' will generate a single opcode.)
*/
// 创建一个OP_LOADNIL指令，但尝试进行优化：如果前一条指令也是OP_LOADNIL并且范围兼容，
// 则调整前一条指令的范围，而不是生成一条新指令。（例如，'local a; local b' 将生成单个操作码。）
// from寄存器开始
// n个寄存器
void luaK_nil (FuncState *fs, int from, int n) {
  // 表示需要设置为nil的最后一个寄存器的索引
  int l = from + n - 1;  /* last register to set nil */
  // 当前代码的前一条指令
  Instruction *previous = previousinstruction(fs);
  if (GET_OPCODE(*previous) == OP_LOADNIL) {  /* previous is LOADNIL? */
    // 前一条指令的起始寄存器索引
    int pfrom = GETARG_A(*previous);  /* get previous range */
    // 前一条指令的最后一个寄存器索引
    int pl = pfrom + GETARG_B(*previous);
    // 如果当前指令的范围与前一条指令的范围有重叠或相邻，则可以合并
    // 前一条指令的范围是R3-R4，当前指令的范围是R4-R5
    // 前一条指令的范围是R3-R5，当前指令的范围是R4-R6
    if ((pfrom <= from && from <= pl + 1) ||
        (from <= pfrom && pfrom <= l + 1)) {  /* can connect both? */
      // 将当前指令的起始寄存器索引from设置为from和pfrom的最小值
      if (pfrom < from) from = pfrom;  /* from = min(from, pfrom) */
      // 将当前指令的最后一个寄存器索引l设置为l和pl的最大值
      if (pl > l) l = pl;  /* l = max(l, pl) */
      SETARG_A(*previous, from);
      SETARG_B(*previous, l - from);
      return;
    }  /* else go through */
  }
  luaK_codeABC(fs, OP_LOADNIL, from, n - 1, 0);  /* else no optimization */
}


/*
** Gets the destination address of a jump instruction. Used to traverse
** a list of jumps.
*/
// 获取跳转指令的目标地址
static int getjump (FuncState *fs, int pc) {
  int offset = GETARG_sJ(fs->f->code[pc]);
  if (offset == NO_JUMP)  /* point to itself represents end of list */
    // 跳转链表结束
    return NO_JUMP;  /* end of list */
  else
    // 跳转链表
    return (pc+1)+offset;  /* turn offset into absolute position */
}


/*
** Fix jump instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua)
*/
// 将一条跳转指令的目标地址更新为指定的目标地址
static void fixjump (FuncState *fs, int pc, int dest) {
  Instruction *jmp = &fs->f->code[pc];
  // 偏移量
  int offset = dest - (pc + 1);
  lua_assert(dest != NO_JUMP);
  // 顺便检查一下范围哦
  if (!(-OFFSET_sJ <= offset && offset <= MAXARG_sJ - OFFSET_sJ))
    luaX_syntaxerror(fs->ls, "control structure too long");
  lua_assert(GET_OPCODE(*jmp) == OP_JMP);
  SETARG_sJ(*jmp, offset);
}


/*
** Concatenate jump-list 'l2' into jump-list 'l1'
*/
// 将两个跳转链表l1和l2合并为一个链表，以便在后续的编译阶段通过“回填”技术（Backpatching）正确地修补跳转地址
// 
// 跳转位置实际上在生成跳转语句的时候经常是不知道的
// 在编译原理的理论中,使用一种称为”回填”(backpatch)的技术来进行处理.
// 它的做法是,生成跳转语句时,将当前还不知道位置,但是都要跳转到同一个位置的语句链接在一起,
// 形成一个空悬跳转语句的链表,在后面找到跳转位置时,再将跳转位置遍历之前的链表填充回去.
// 
// 比如A,B,C三个OP_JMP指令最后都是跳转到同一个目的地址,而生成这几条指令的时候最终的目的地址并不知道,
// 那么会首先将A的跳转地址设置为B指令的偏移量,同理B指令的跳转地址设置为C指令的偏移量,而在这个跳转链表的最后一个元素C指令,
// 其跳转地址设置为NO_JUMP(-1), 表示它是链表的最后一个元素.
// 
// l1是空悬链表的第一个指令位置
// l2是待加入该链表的指令位置
void luaK_concat (FuncState *fs, int *l1, int l2) {
  // 如果l2是NO_JUMP,直接返回,因为这个位置存储的指令不是一个跳转指令
  if (l2 == NO_JUMP) return;  /* nothing to concatenate? */
  // 如果l1是NO_JUMP,说明这个跳转链表为空当前没有空悬的跳转指令在该链表中,直接赋值为l2;
  else if (*l1 == NO_JUMP)  /* no original list? */
    *l1 = l2;  /* 'l1' points to 'l2' */
  // 说明l1现在是一个非空的跳转链表,首先遍历这个链表到最后一个元素,
  // 其判定标准是跳转位置为NO_JUMP时表示是跳转链表的最后一个元素,
  // 然后调用fixjump函数将最后一个元素的跳转位置设置为l2,这样l2就添加到了该跳转链表中.
  else {
    int list = *l1;
    int next;
    while ((next = getjump(fs, list)) != NO_JUMP)  /* find last element */
      list = next;
    fixjump(fs, list, l2);  /* last element links to 'l2' */
  }
}


/*
** Create a jump instruction and return its position, so its destination
** can be fixed later (with 'fixjump').
*/
// 生成跳转指令，初始时跳转目标未确定，需要在后续解析过程中通过fixjump修正
int luaK_jump (FuncState *fs) {
  return codesJ(fs, OP_JMP, NO_JUMP, 0);
}


/*
** Code a 'return' instruction
*/
// 生成一条RETURN指令
void luaK_ret (FuncState *fs, int first, int nret) {
  OpCode op;
  switch (nret) {
    case 0: op = OP_RETURN0; break;
    case 1: op = OP_RETURN1; break;
    // return R[A], ... ,R[A+B-2]
    default: op = OP_RETURN; break;
  }
  luaK_codeABC(fs, op, first, nret + 1, 0);
}


/*
** Code a "conditional jump", that is, a test or comparison opcode
** followed by a jump. Return jump position.
*/
// 生成条件&跳转指令，返回跳转指令的位置
static int condjump (FuncState *fs, OpCode op, int A, int B, int C, int k) {
  // 生成条件指令
  luaK_codeABCk(fs, op, A, B, C, k);
  // 生成跳转指令
  return luaK_jump(fs);
}


/*
** returns current 'pc' and marks it as a jump target (to avoid wrong
** optimizations with consecutive instructions not in the same basic block).
*/
// 返回当前的pc并将其标记为跳转目标（以避免连续指令不在同一基本块时发生错误优化）
int luaK_getlabel (FuncState *fs) {
  fs->lasttarget = fs->pc;
  return fs->pc;
}


/*
** Returns the position of the instruction "controlling" a given
** jump (that is, its condition), or the jump itself if it is
** unconditional.
*/
// 找到与某个跳转指令相关联的控制指令（即条件判断指令）
// 或者返回跳转指令本身（如果它是无条件跳转）
static Instruction *getjumpcontrol (FuncState *fs, int pc) {
  Instruction *pi = &fs->f->code[pc];
  // 前一条指令是否是条件判断指令
  if (pc >= 1 && testTMode(GET_OPCODE(*(pi-1))))
    return pi-1;
  else
    return pi;
}


/*
** Patch destination register for a TESTSET instruction.
** If instruction in position 'node' is not a TESTSET, return 0 ("fails").
** Otherwise, if 'reg' is not 'NO_REG', set it as the destination
** register. Otherwise, change instruction to a simple 'TEST' (produces
** no register value)
*/
// 修改跳转控制指令（TESTSET）的目标寄存器，或者将其转换为简单的测试指令（TEST）
// node指令的位置
// reg目标寄存器的索引（如果为NO_REG，则表示不需要存储结果）
// 返回是否修补
// OP_TESTSET修改R[A]或者转成OP_TEST，返回1，否则返回0
// 只要跳转指令前面的指令是OP_TESTSET，肯定就返回1
static int patchtestreg (FuncState *fs, int node, int reg) {
  Instruction *i = getjumpcontrol(fs, node);
  if (GET_OPCODE(*i) != OP_TESTSET)
    return 0;  /* cannot patch other instructions */
  // 如果需要存储结果（reg != NO_REG），并且目标寄存器与当前寄存器不同（reg != GETARG_B(*i)），则修改目标寄存器
  if (reg != NO_REG && reg != GETARG_B(*i))
    SETARG_A(*i, reg);
  else {
    // 如果不需要存储结果（reg == NO_REG），或者目标寄存器已经是当前寄存器（reg == GETARG_B(*i)），则将指令改为 TEST：
     /* no register to put value or register already has the value;
        change instruction to simple test */
    *i = CREATE_ABCk(OP_TEST, GETARG_B(*i), 0, 0, GETARG_k(*i));
  }
  return 1;
}


/*
** Traverse a list of tests ensuring no one produces a value
*/
// 把跳转列表中的OP_TESTSET转化成OP_TEST指令
static void removevalues (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list))
      patchtestreg(fs, list, NO_REG);
}


/*
** Traverse a list of tests, patching their destination address and
** registers: tests producing values jump to 'vtarget' (and put their
** values in 'reg'), other tests jump to 'dtarget'.
*/
// 跳转位置回填
// 遍历一个跳转链表的所有元素,调用fixjump函数将跳转地址回填到链表中的每个指令中
// list跳转列表的头节点索引，指令下标
// vtarget分支1需要跳转的指令下标
// reg存储结果的寄存器索引
// dtarget分支2需要跳转的指令下标
static void patchlistaux (FuncState *fs, int list, int vtarget, int reg,
                          int dtarget) {
  // 循环遍历跳转链表，直到链表为空
  while (list != NO_JUMP) {
    // 获取当前跳转指令的下一条跳转指令的索引
    int next = getjump(fs, list);
    // 检查list上一个指令是否是OP_TESTSET指令，只要是就返回1
    if (patchtestreg(fs, list, reg))
      // 将list修补为vtarget
      fixjump(fs, list, vtarget);
    else
      // 将list修补为dtarget
      fixjump(fs, list, dtarget);  /* jump to default target */
    list = next;
  }
}


/*
** Path all jumps in 'list' to jump to 'target'.
** (The assert means that we cannot fix a jump to a forward address
** because we only know addresses once code is generated.)
*/
// 将待匹配的标签更新为指定标签的地址
// list跳转列表的头节点索引，指令下标
// target目标地址，表示跳转指令应跳转到的位置，指令下标
void luaK_patchlist (FuncState *fs, int list, int target) {
  lua_assert(target <= fs->pc);
  patchlistaux(fs, list, target, NO_REG, target);
}


// 将一个跳转列表（Jump List）中的所有跳转指令的目标地址设置为当前代码的位置
void luaK_patchtohere (FuncState *fs, int list) {
  // 获取当前代码位置的标签，标记“这里”为跳转目标
  int hr = luaK_getlabel(fs);  /* mark "here" as a jump target */
  // 将跳转列表中的所有跳转指令修补到当前位置
  luaK_patchlist(fs, list, hr);
}


/* limit for difference between lines in relative line info. */
// ~128~127
#define LIMLINEDIFF	0x80


/*
** Save line info for a new instruction. If difference from last line
** does not fit in a byte, of after that many instructions, save a new
** absolute line info; (in that case, the special value 'ABSLINEINFO'
** in 'lineinfo' signals the existence of this absolute information.)
** Otherwise, store the difference from last line in 'lineinfo'.
*/
// 保存新的指令的行信息
static void savelineinfo (FuncState *fs, Proto *f, int line) {
  // 新增指令和上一个指令行号差值
  int linedif = line - fs->previousline;
  // 新增指令下标
  int pc = fs->pc - 1;  /* last instruction coded */
  // 如果行号差值的绝对值大于等于LIMLINEDIFF，则需要使用绝对行号
  // 如果连续使用相对行号的次数（fs->iwthabs）达到 MAXIWTHABS，也需要使用绝对行号
  if (abs(linedif) >= LIMLINEDIFF || fs->iwthabs++ >= MAXIWTHABS) {
    luaM_growvector(fs->ls->L, f->abslineinfo, fs->nabslineinfo,
                    f->sizeabslineinfo, AbsLineInfo, MAX_INT, "lines");
    f->abslineinfo[fs->nabslineinfo].pc = pc;
    f->abslineinfo[fs->nabslineinfo++].line = line;
    linedif = ABSLINEINFO;  /* signal that there is absolute information */
    fs->iwthabs = 1;  /* restart counter */
  }
  luaM_growvector(fs->ls->L, f->lineinfo, pc, f->sizelineinfo, ls_byte,
                  MAX_INT, "opcodes");
  // 新增指令下标 对应 新增指令和上一个指令行号差值
  f->lineinfo[pc] = linedif;
  // 记录本次指令所在行号
  fs->previousline = line;  /* last line saved */
}


/*
** Remove line information from the last instruction.
** If line information for that instruction is absolute, set 'iwthabs'
** above its max to force the new (replacing) instruction to have
** absolute line info, too.
*/
// 
static void removelastlineinfo (FuncState *fs) {
  Proto *f = fs->f;
  int pc = fs->pc - 1;  /* last instruction coded */
  if (f->lineinfo[pc] != ABSLINEINFO) {  /* relative line info? */
    fs->previousline -= f->lineinfo[pc];  /* correct last line saved */
    fs->iwthabs--;  /* undo previous increment */
  }
  else {  /* absolute line information */
    lua_assert(f->abslineinfo[fs->nabslineinfo - 1].pc == pc);
    fs->nabslineinfo--;  /* remove it */
    fs->iwthabs = MAXIWTHABS + 1;  /* force next line info to be absolute */
  }
}


/*
** Remove the last instruction created, correcting line information
** accordingly.
*/
static void removelastinstruction (FuncState *fs) {
  removelastlineinfo(fs);
  fs->pc--;
}


/*
** Emit instruction 'i', checking for array sizes and saving also its
** line information. Return 'i' position.
*/
// 指令放入fs->f->code，返回的是当前指令下标
int luaK_code (FuncState *fs, Instruction i) {
  Proto *f = fs->f;
  /* put new instruction in code array */
  luaM_growvector(fs->ls->L, f->code, fs->pc, f->sizecode, Instruction,
                  MAX_INT, "opcodes");
  f->code[fs->pc++] = i;
  savelineinfo(fs, f, fs->ls->lastline);
  return fs->pc - 1;  /* index of new instruction */
}


/*
** Format and emit an 'iABC' instruction. (Assertions check consistency
** of parameters versus opcode.)
*/
// 产生iABC指令到fs->f—>code中
int luaK_codeABCk (FuncState *fs, OpCode o, int a, int b, int c, int k) {
  lua_assert(getOpMode(o) == iABC);
  lua_assert(a <= MAXARG_A && b <= MAXARG_B &&
             c <= MAXARG_C && (k & ~1) == 0);
  return luaK_code(fs, CREATE_ABCk(o, a, b, c, k));
}


/*
** Format and emit an 'iABx' instruction.
*/
// 生成iABx指令
int luaK_codeABx (FuncState *fs, OpCode o, int a, unsigned int bc) {
  lua_assert(getOpMode(o) == iABx);
  lua_assert(a <= MAXARG_A && bc <= MAXARG_Bx);
  return luaK_code(fs, CREATE_ABx(o, a, bc));
}


/*
** Format and emit an 'iAsBx' instruction.
*/
// 生成iAsBx指令
static int codeAsBx (FuncState *fs, OpCode o, int a, int bc) {
  unsigned int b = bc + OFFSET_sBx;
  lua_assert(getOpMode(o) == iAsBx);
  lua_assert(a <= MAXARG_A && b <= MAXARG_Bx);
  return luaK_code(fs, CREATE_ABx(o, a, b));
}


/*
** Format and emit an 'isJ' instruction.
*/
// 生成isJ指令
static int codesJ (FuncState *fs, OpCode o, int sj, int k) {
  unsigned int j = sj + OFFSET_sJ;
  lua_assert(getOpMode(o) == isJ);
  lua_assert(j <= MAXARG_sJ && (k & ~1) == 0);
  return luaK_code(fs, CREATE_sJ(o, j, k));
}


/*
** Emit an "extra argument" instruction (format 'iAx')
*/
// 处理超出字节码指令常规参数范围的情框
static int codeextraarg (FuncState *fs, int a) {
  lua_assert(a <= MAXARG_Ax);
  return luaK_code(fs, CREATE_Ax(OP_EXTRAARG, a));
}


/*
** Emit a "load constant" instruction, using either 'OP_LOADK'
** (if constant index 'k' fits in 18 bits) or an 'OP_LOADKX'
** instruction with "extra argument".
*/
// 生成OP_LOADK/OP_LOADKX
static int luaK_codek (FuncState *fs, int reg, int k) {
  if (k <= MAXARG_Bx)
    return luaK_codeABx(fs, OP_LOADK, reg, k);
  else {
    int p = luaK_codeABx(fs, OP_LOADKX, reg, 0);
    codeextraarg(fs, k);
    return p;
  }
}


/*
** Check register-stack level, keeping track of its maximum size
** in field 'maxstacksize'
*/
// 确保maxstacksize可以容纳n个元素
void luaK_checkstack (FuncState *fs, int n) {
  int newstack = fs->freereg + n;
  if (newstack > fs->f->maxstacksize) {
    if (newstack >= MAXREGS)
      luaX_syntaxerror(fs->ls,
        "function or expression needs too many registers");
    fs->f->maxstacksize = cast_byte(newstack);
  }
}


/*
** Reserve 'n' registers in register stack
*/
// 寄存器栈上预分配n个
void luaK_reserveregs (FuncState *fs, int n) {
  luaK_checkstack(fs, n);
  fs->freereg += n;
}


/*
** Free register 'reg', if it is neither a constant index nor
** a local variable.
)
*/
// 释放不再使用的寄存器，以优化寄存器的使用效率
static void freereg (FuncState *fs, int reg) {
  // luaY_nvarstack(fs)返回当前函数中活动变量所占用的寄存器范围。
  // 如果寄存器reg的索引大于等于这个范围，说明它不是活动变量的一部分，因此可以被释放
  if (reg >= luaY_nvarstack(fs)) {
    fs->freereg--;
    lua_assert(reg == fs->freereg);
  }
}


/*
** Free two registers in proper order
*/
// 按照正确的顺序释放两个寄存器资源
static void freeregs (FuncState *fs, int r1, int r2) {
  if (r1 > r2) {
    freereg(fs, r1);
    freereg(fs, r2);
  }
  else {
    freereg(fs, r2);
    freereg(fs, r1);
  }
}


/*
** Free register used by expression 'e' (if any)
*/
// 释放不需要的寄存器
static void freeexp (FuncState *fs, expdesc *e) {
  if (e->k == VNONRELOC)
    freereg(fs, e->u.info);
}


/*
** Free registers used by expressions 'e1' and 'e2' (if any) in proper
** order.
*/
// 释放表达式e1和e2使用的寄存器资源
static void freeexps (FuncState *fs, expdesc *e1, expdesc *e2) {
  // 检查左侧表达式eX是否是一个不可重定位的值（即它已经在寄存器中）
  // 如果是，则获取其寄存器索引eX->u.info
  // 如果不是，则设置为-1，表示没有寄存器需要释放
  int r1 = (e1->k == VNONRELOC) ? e1->u.info : -1;
  int r2 = (e2->k == VNONRELOC) ? e2->u.info : -1;
  freeregs(fs, r1, r2);
}


/*
** Add constant 'v' to prototype's list of constants (field 'k').
** Use scanner's table to cache position of constants in constant list
** and try to reuse constants. Because some values should not be used
** as keys (nil cannot be a key, integer keys can collapse with float
** keys), the caller must provide a useful 'key' for indexing the cache.
** Note that all functions share the same table, so entering or exiting
** a function can make some indices wrong.
*/
// 将常量v加入到函数原型常量表中
// key表示常量的键
// v表示常量的值
static int addk (FuncState *fs, TValue *key, TValue *v) {
  TValue val;
  lua_State *L = fs->ls->L;
  Proto *f = fs->f;
  // 看看缓存中是否有
  const TValue *idx = luaH_get(fs->ls->h, key);  /* query scanner table */
  int k, oldsize;
  if (ttisinteger(idx)) {  /* is there an index there? */
    // 找到了，判断一下，没问题就重用了
    k = cast_int(ivalue(idx));
    /* correct value? (warning: must distinguish floats from integers!) */
    if (k < fs->nk && ttypetag(&f->k[k]) == ttypetag(v) &&
                      luaV_rawequalobj(&f->k[k], v))
      return k;  /* reuse index */
  }
  /* constant not found; create a new entry */
  oldsize = f->sizek;
  k = fs->nk;
  /* numerical value does not need GC barrier;
     table has no metatable, so it does not need to invalidate cache */
  setivalue(&val, k);
  // 缓存起来吧
  luaH_finishset(L, fs->ls->h, key, idx, &val);
  // 开始设置
  luaM_growvector(L, f->k, k, f->sizek, TValue, MAXARG_Ax, "constants");
  while (oldsize < f->sizek) setnilvalue(&f->k[oldsize++]);
  setobj(L, &f->k[k], v);
  fs->nk++;
  luaC_barrier(L, f, v);
  return k;
}


/*
** Add a string to list of constants and return its index.
*/
// 字符串加入常量表，并且返回索引
static int stringK (FuncState *fs, TString *s) {
  TValue o;
  setsvalue(fs->ls->L, &o, s);
  return addk(fs, &o, &o);  /* use string itself as key */
}


/*
** Add an integer to list of constants and return its index.
*/
// 整数加入常量表，并且返回索引
static int luaK_intK (FuncState *fs, lua_Integer n) {
  TValue o;
  setivalue(&o, n);
  return addk(fs, &o, &o);  /* use integer itself as key */
}

/*
** Add a float to list of constants and return its index. Floats
** with integral values need a different key, to avoid collision
** with actual integers. To that, we add to the number its smaller
** power-of-two fraction that is still significant in its scale.
** For doubles, that would be 1/2^52.
** (This method is not bulletproof: there may be another float
** with that value, and for floats larger than 2^53 the result is
** still an integer. At worst, this only wastes an entry with
** a duplicate.)
*/
// 浮点数加入常量表，并且返回索引
static int luaK_numberK (FuncState *fs, lua_Number r) {
  TValue o;
  lua_Integer ik;
  setfltvalue(&o, r);
  if (!luaV_flttointeger(r, &ik, F2Ieq))  /* not an integral value? */
    return addk(fs, &o, &o);  /* use number itself as key */
  else {  /* must build an alternative key */
    const int nbm = l_floatatt(MANT_DIG);
    const lua_Number q = l_mathop(ldexp)(l_mathop(1.0), -nbm + 1);
    const lua_Number k = (ik == 0) ? q : r + r*q;  /* new key */
    TValue kv;
    setfltvalue(&kv, k);
    /* result is not an integral value, unless value is too large */
    lua_assert(!luaV_flttointeger(k, &ik, F2Ieq) ||
                l_mathop(fabs)(r) >= l_mathop(1e6));
    return addk(fs, &kv, &o);
  }
}


/*
** Add a false to list of constants and return its index.
*/
// false加入常量表，并且返回索引
static int boolF (FuncState *fs) {
  TValue o;
  setbfvalue(&o);
  return addk(fs, &o, &o);  /* use boolean itself as key */
}


/*
** Add a true to list of constants and return its index.
*/
// true加入常量表，并且返回索引
static int boolT (FuncState *fs) {
  TValue o;
  setbtvalue(&o);
  return addk(fs, &o, &o);  /* use boolean itself as key */
}


/*
** Add nil to list of constants and return its index.
*/
// nil加入常量表，并且返回索引
static int nilK (FuncState *fs) {
  TValue k, v;
  setnilvalue(&v);
  /* cannot use nil as key; instead use table itself to represent nil */
  sethvalue(fs->ls->L, &k, fs->ls->h);
  return addk(fs, &k, &v);
}


/*
** Check whether 'i' can be stored in an 'sC' operand. Equivalent to
** (0 <= int2sC(i) && int2sC(i) <= MAXARG_C) but without risk of
** overflows in the hidden addition inside 'int2sC'.
*/
// 检查一个整数i是否可以存储在一个sC类型的操作数中
static int fitsC (lua_Integer i) {
  return (l_castS2U(i) + OFFSET_sC <= cast_uint(MAXARG_C));
}


/*
** Check whether 'i' can be stored in an 'sBx' operand.
*/
// i是否可以放入sBx
static int fitsBx (lua_Integer i) {
  return (-OFFSET_sBx <= i && i <= MAXARG_Bx - OFFSET_sBx);
}


// 生成加载整数的指令
void luaK_int (FuncState *fs, int reg, lua_Integer i) {
  if (fitsBx(i))
    // 如果整数适合sBx字段，则生成一条OP_LOADI指令。
    // OP_LOADI是一条专门用于加载小整数的指令，它直接将整数值嵌入到指令中，避免了使用常量表
    codeAsBx(fs, OP_LOADI, reg, cast_int(i));
  else
    // 没辙了，只能OP_LOADK/OP_LOADKX
    luaK_codek(fs, reg, luaK_intK(fs, i));
}


static void luaK_float (FuncState *fs, int reg, lua_Number f) {
  lua_Integer fi;
  if (luaV_flttointeger(f, &fi, F2Ieq) && fitsBx(fi))
    codeAsBx(fs, OP_LOADF, reg, cast_int(fi));
  else
    luaK_codek(fs, reg, luaK_numberK(fs, f));
}


/*
** Convert a constant in 'v' into an expression description 'e'
*/
// 用于将一个常量值转换为表达式描述信息
static void const2exp (TValue *v, expdesc *e) {
  switch (ttypetag(v)) {
    case LUA_VNUMINT:
      e->k = VKINT; e->u.ival = ivalue(v);
      break;
    case LUA_VNUMFLT:
      e->k = VKFLT; e->u.nval = fltvalue(v);
      break;
    case LUA_VFALSE:
      e->k = VFALSE;
      break;
    case LUA_VTRUE:
      e->k = VTRUE;
      break;
    case LUA_VNIL:
      e->k = VNIL;
      break;
    case LUA_VSHRSTR:  case LUA_VLNGSTR:
      e->k = VKSTR; e->u.strval = tsvalue(v);
      break;
    default: lua_assert(0);
  }
}


/*
** Fix an expression to return the number of results 'nresults'.
** 'e' must be a multi-ret expression (function call or vararg).
*/
// 修改字节码指令，确保表达式返回指定数量的结果
void luaK_setreturns (FuncState *fs, expdesc *e, int nresults) {
  Instruction *pc = &getinstruction(fs, e);
  if (e->k == VCALL)  /* expression is an open function call? */
    // 函数调用表达式
    // OP_CALL指令中返回值数量修改为nresults
    SETARG_C(*pc, nresults + 1);
  else {
    // 可变参数表达式
    lua_assert(e->k == VVARARG);
    // OP_VARARG指令中返回结果数量修改为nresults
    SETARG_C(*pc, nresults + 1);
    // OP_VARARG指令中结果存储的起始寄存器修改为当前可用fs->freereg
    SETARG_A(*pc, fs->freereg);
    // 预留一个寄存器，用于存储可变参数的起始结果
    luaK_reserveregs(fs, 1);
  }
}


/*
** Convert a VKSTR to a VK
*/
// 将VKSTR转换成VK
static void str2K (FuncState *fs, expdesc *e) {
  lua_assert(e->k == VKSTR);
  e->u.info = stringK(fs, e->u.strval);
  e->k = VK;
}


/*
** Fix an expression to return one result.
** If expression is not a multi-ret expression (function call or
** vararg), it already returns one result, so nothing needs to be done.
** Function calls become VNONRELOC expressions (as its result comes
** fixed in the base register of the call), while vararg expressions
** become VRELOC (as OP_VARARG puts its results where it wants).
** (Calls are created returning one result, so that does not need
** to be fixed.)
*/
// 将表达式的结果限制为单个返回值
void luaK_setoneret (FuncState *fs, expdesc *e) {
  if (e->k == VCALL) {  /* expression is an open function call? */
    // 如果表达式类型是VCALL，表示这是一个函数调用
    /* already returns 1 value */
    // 断言检查：函数调用的返回值数量（由C参数决定）必须为2。
    // 2表示一个函数调用返回一个结果
    // 生成函数调用指令会假设函数返回一个值
    lua_assert(GETARG_C(getinstruction(fs, e)) == 2);
    // 将表达式类型设置为VNONRELOC，表示结果固定在某个寄存器中
    e->k = VNONRELOC;  /* result has fixed position */
    // 获取函数寄存器位置，其实就是调用完以后第一个参数的位置
    e->u.info = GETARG_A(getinstruction(fs, e));
  }
  else if (e->k == VVARARG) {
    // 如果表达式类型是VVARARG，表示这是一个可变参数表达式
    // 设置OP_VARARG指令的C参数为2，表示只返回一个结果
    SETARG_C(getinstruction(fs, e), 2);
    // 因为是可变参数，无法确定数量和位置
    // 所以这里设置为：⽬标寄存器未定
    e->k = VRELOC;  /* can relocate its simple result */
  }
}


/*
** Ensure that expression 'e' is not a variable (nor a <const>).
** (Expression still may have jump lists.)
*/
// 将表达式e的值“提取”出来，转换为一种可以直接使用的形式（如寄存器中的值或常量），以便后续操作
// 作用范围: 处理表达式的内部结构，确保其值可以被后续字节码生成逻辑直接使用
// 本质: 这是一个“预处理”函数，主要用于规范化表达式的形式
void luaK_dischargevars (FuncState *fs, expdesc *e) {
  switch (e->k) {
    case VCONST: {
      // 已经编译优化过的常量表达式
      const2exp(const2val(fs, e), e);
      break;
    }
    case VLOCAL: {  /* already in a register */
      // 本地变量肯定已经在寄存器中了
      int temp = e->u.var.ridx;
      e->u.info = temp;  /* (can't do a direct assignment; values overlap) */
      e->k = VNONRELOC;  /* becomes a non-relocatable value */
      break;
    }
    case VUPVAL: {  /* move value to some (pending) register */
      // 生成一条OP_GETUPVAL指令，将其值加载到寄存器中
      e->u.info = luaK_codeABC(fs, OP_GETUPVAL, 0, e->u.info, 0);
      // 表示该值需要重定位到某个寄存器
      e->k = VRELOC;
      break;
    }
    case VINDEXUP: {
      // 如果表达式是一个上值索引，生成一条OP_GETTABUP指令，将其值加载到寄存器中
      // 注意这里没有设置A的值，是需要被重定向到的寄存器(栈中)
      e->u.info = luaK_codeABC(fs, OP_GETTABUP, 0, e->u.ind.t, e->u.ind.idx);
      // 同上
      e->k = VRELOC;
      break;
    }
    case VINDEXI: {
      // 释放掉额外的寄存器
      freereg(fs, e->u.ind.t);
      // 如果表达式是一个整数索引，生成一条OP_GETI指令，将其值加载到寄存器中
      e->u.info = luaK_codeABC(fs, OP_GETI, 0, e->u.ind.t, e->u.ind.idx);
      // 同上
      e->k = VRELOC;
      break;
    }
    case VINDEXSTR: {
      // 同上
      freereg(fs, e->u.ind.t);
      // 如果表达式是一个字符串索引，生成一条OP_GETFIELD指令，将其值加载到寄存器中
      e->u.info = luaK_codeABC(fs, OP_GETFIELD, 0, e->u.ind.t, e->u.ind.idx);
      // 同上
      e->k = VRELOC;
      break;
    }
    case VINDEXED: {
      // 同上
      freeregs(fs, e->u.ind.t, e->u.ind.idx);
      // 如果表达式是一个通用索引，生成一条OP_GETTABLE指令，将其值加载到寄存器中
      e->u.info = luaK_codeABC(fs, OP_GETTABLE, 0, e->u.ind.t, e->u.ind.idx);
      // 同上
      e->k = VRELOC;
      break;
    }
    case VVARARG: case VCALL: {
      // 将可变参数或函数调用的结果限制为单个返回值
      luaK_setoneret(fs, e);
      break;
    }
    default: break;  /* there is one value available (somewhere) */
  }
}


/*
** Ensure expression value is in register 'reg', making 'e' a
** non-relocatable expression.
** (Expression still may have jump lists.)
*/
// 根据不同的表达式类型（NIL,布尔表达式，数字等等）来生成存取表达式的值到寄存器的opcode
static void discharge2reg (FuncState *fs, expdesc *e, int reg) {
  // 本质：表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VNIL: {
      luaK_nil(fs, reg, 1);
      break;
    }
    case VFALSE: {
      luaK_codeABC(fs, OP_LOADFALSE, reg, 0, 0);
      break;
    }
    case VTRUE: {
      luaK_codeABC(fs, OP_LOADTRUE, reg, 0, 0);
      break;
    }
    case VKSTR: {
      str2K(fs, e);
    }  /* FALLTHROUGH */
    case VK: {
      luaK_codek(fs, reg, e->u.info);
      break;
    }
    case VKFLT: {
      luaK_float(fs, reg, e->u.nval);
      break;
    }
    case VKINT: {
      luaK_int(fs, reg, e->u.ival);
      break;
    }
    case VRELOC: {
      // 需要重定向，栈下标写入指令A域
      Instruction *pc = &getinstruction(fs, e);
      SETARG_A(*pc, reg);  /* instruction will put result in 'reg' */
      break;
    }
    case VNONRELOC: {
      if (reg != e->u.info)
        // R[A] := R[B]
        luaK_codeABC(fs, OP_MOVE, reg, e->u.info, 0);
      break;
    }
    default: {
      lua_assert(e->k == VJMP);
      return;  /* nothing to do... */
    }
  }
  e->u.info = reg;
  e->k = VNONRELOC;
}


/*
** Ensure expression value is in a register, making 'e' a
** non-relocatable expression.
** (Expression still may have jump lists.)
*/
// 将表达式的值加载到一个寄存器中，并将表达式标记为不可重定位（VNONRELOC）
static void discharge2anyreg (FuncState *fs, expdesc *e) {
  if (e->k != VNONRELOC) {  /* no fixed register yet? */
    luaK_reserveregs(fs, 1);  /* get a register */
    discharge2reg(fs, e, fs->freereg-1);  /* put value there */
  }
}


static int code_loadbool (FuncState *fs, int A, OpCode op) {
  luaK_getlabel(fs);  /* those instructions may be jump targets */
  return luaK_codeABC(fs, op, A, 0, 0);
}


/*
** check whether list has any jump that do not produce a value
** or produce an inverted value
*/
static int need_value (FuncState *fs, int list) {
  for (; list != NO_JUMP; list = getjump(fs, list)) {
    Instruction i = *getjumpcontrol(fs, list);
    if (GET_OPCODE(i) != OP_TESTSET) return 1;
  }
  return 0;  /* not found */
}


/*
** Ensures final expression result (which includes results from its
** jump lists) is in register 'reg'.
** If expression has jumps, need to patch these jumps either to
** its final position or to "load" instructions (for those tests
** that do not produce values).
*/
// 把表达式的数据放入寄存器空间的工作
static void exp2reg (FuncState *fs, expdesc *e, int reg) {
  // 根据不同的表达式类型（NIL,布尔表达式，数字等等）来生成存取表达式的值到寄存器的opcode
  discharge2reg(fs, e, reg);
  if (e->k == VJMP)  /* expression itself is a test? */
    luaK_concat(fs, &e->t, e->u.info);  /* put this jump in 't' list */
  if (hasjumps(e)) {
    int final;  /* position after whole expression */
    int p_f = NO_JUMP;  /* position of an eventual LOAD false */
    int p_t = NO_JUMP;  /* position of an eventual LOAD true */
    if (need_value(fs, e->t) || need_value(fs, e->f)) {
      int fj = (e->k == VJMP) ? NO_JUMP : luaK_jump(fs);
      p_f = code_loadbool(fs, reg, OP_LFALSESKIP);  /* skip next inst. */
      p_t = code_loadbool(fs, reg, OP_LOADTRUE);
      /* jump around these booleans if 'e' is not a test */
      luaK_patchtohere(fs, fj);
    }
    final = luaK_getlabel(fs);
    patchlistaux(fs, e->f, final, reg, p_f);
    patchlistaux(fs, e->t, final, reg, p_t);
  }
  e->f = e->t = NO_JUMP;
  e->u.info = reg;
  e->k = VNONRELOC;
}


/*
** Ensures final expression result is in next available register.
*/
// 确保最终生成的表达式值在下一个可用的寄存器中(栈中)
// 当然这里以生成指令来做到这一点
void luaK_exp2nextreg (FuncState *fs, expdesc *e) {
  // 表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e);
  // 有需要释放的释放
  freeexp(fs, e);
  // 预分配可用的函数寄存器空间，得到这个空间对应的寄存器索引，有了空间才能存储变量
  luaK_reserveregs(fs, 1);
  // 把表达式的数据放入寄存器空间的工作
  exp2reg(fs, e, fs->freereg - 1);
}


/*
** Ensures final expression result is in some (any) register
** and return that register.
*/
// 确保表达式的结果被加载到某个寄存器中，并返回该寄存器的索引
// 作用范围: 不仅规范化表达式的形式，还确保其值最终存储在寄存器中，并返回寄存器索引
// 本质: 这是一个“强制加载”函数，确保表达式的结果在寄存器中可用
int luaK_exp2anyreg (FuncState *fs, expdesc *e) {
  // 表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e);
  // 表达式的值已经固定在某个寄存器中，不需要重新定位
  if (e->k == VNONRELOC) {  /* expression already has a register? */
    if (!hasjumps(e))  /* no jumps? */
      // 如果没有跳转指令，则表达式的结果已经在一个寄存器中，直接返回该寄存器的索引
      return e->u.info;  /* result is already in a register */
    // 说明有跳转
    if (e->u.info >= luaY_nvarstack(fs)) {  /* reg. is not a local? */
      // 寄存器不是局部变量的情况
      // 
      exp2reg(fs, e, e->u.info);  /* put final result in it */
      return e->u.info;
    }
    /* else expression has jumps and cannot change its register
       to hold the jump values, because it is a local variable.
       Go through to the default case. */
  }
  // 将表达式的结果加载到下一个可用的寄存器中
  luaK_exp2nextreg(fs, e);  /* default: use next available register */
  return e->u.info;
}


/*
** Ensures final expression result is either in a register
** or in an upvalue.
*/
// 确保表达式最终结果是在一个寄存器或在一个上值中
void luaK_exp2anyregup (FuncState *fs, expdesc *e) {
  // 如果表达式的类型不是VUPVAL，则需要将其转换为寄存器
  // 如果表达式包含跳转，则需要将其转换为寄存器，因为跳转无法直接操作上值
  // 在Lua的编译器中，如果一个表达式包含跳转（例如条件分支或循环），那么它的结果必须存储在寄存器中，而不能直接使用上值（upvalue）
  if (e->k != VUPVAL || hasjumps(e))
    luaK_exp2anyreg(fs, e);
}


/*
** Ensures final expression result is either in a register
** or it is a constant.
*/
// ​确保表达式的最终结果存储在寄存器中或是一个常量
void luaK_exp2val (FuncState *fs, expdesc *e) {
  if (hasjumps(e))
    // 如果表达式有跳转（例如条件跳转或循环跳转），将表达式的结果加载到寄存器中
    luaK_exp2anyreg(fs, e);
  else
    // 将表达式的结果从变量中“释放”，使其成为常量或寄存器中的值
    luaK_dischargevars(fs, e);
}


/*
** Try to make 'e' a K expression with an index in the range of R/K
** indices. Return true iff succeeded.
*/
// 尝试将表达式转换为常量表达式
static int luaK_exp2K (FuncState *fs, expdesc *e) {
  if (!hasjumps(e)) {
    int info;
    switch (e->k) {  /* move constants to 'k' */
      case VTRUE: info = boolT(fs); break;
      case VFALSE: info = boolF(fs); break;
      case VNIL: info = nilK(fs); break;
      case VKINT: info = luaK_intK(fs, e->u.ival); break;
      case VKFLT: info = luaK_numberK(fs, e->u.nval); break;
      case VKSTR: info = stringK(fs, e->u.strval); break;
      case VK: info = e->u.info; break;
      default: return 0;  /* not a constant */
    }
    if (info <= MAXINDEXRK) {  /* does constant fit in 'argC'? */
      // 正常常量索引范围就返回
      e->k = VK;  /* make expression a 'K' expression */
      e->u.info = info;
      return 1;
    }
  }
  /* else, expression doesn't fit; leave it unchanged */
  return 0;
}


/*
** Ensures final expression result is in a valid R/K index
** (that is, it is either in a register or in 'k' with an index
** in the range of R/K indices).
** Returns 1 iff expression is K.
*/
// 确保表达式的结果可以被存储在一个有效的R/K索引中，
// 被存储在常量表中，则返回1；否则将表达式的值加载到寄存器中，并返回0
static int exp2RK (FuncState *fs, expdesc *e) {
  if (luaK_exp2K(fs, e))
    // 尝试将表达式转换为常量表达式成功
    return 1;
  else {  /* not a constant in the right range: put it in a register */
    // 将表达式的值加载到任意可用的寄存器中
    luaK_exp2anyreg(fs, e);
    return 0;
  }
}


// 根据操作码o寄存器索引a和b以及表达式描述信息ec，生成一条iABC字节码指令
static void codeABRK (FuncState *fs, OpCode o, int a, int b,
                      expdesc *ec) {
  // 确保表达式ec是在常量或者寄存器中，1代表是常量表中，0代表是寄存器中
  int k = exp2RK(fs, ec);
  luaK_codeABCk(fs, o, a, b, ec->u.info, k);
}


/*
** Generate code to store result of expression 'ex' into variable 'var'.
*/
// ​将表达式ex的结果存储到变量var中
void luaK_storevar (FuncState *fs, expdesc *var, expdesc *ex) {
  switch (var->k) {
    case VLOCAL: {
      // 目标变量是局部变量
      // 如果需要，释放表达式ex占用的寄存器
      freeexp(fs, ex);
      // 将表达式ex的值计放入目标变量所在的寄存器
      exp2reg(fs, ex, var->u.var.ridx);  /* compute 'ex' into proper place */
      return;
    }
    case VUPVAL: {
      // 目标变量是上值
      // 将表达式ex的值加载到一个寄存器中，并返回该寄存器的索引
      int e = luaK_exp2anyreg(fs, ex);
      // UpValue[B] := R[A]
      luaK_codeABC(fs, OP_SETUPVAL, e, var->u.info, 0);
      break;
    }
    case VINDEXUP: {
      // 目标变量是上值表，并且k在常量表中字符串
      // UpValue[A][K[B]:shortstring] := RK(C)
      codeABRK(fs, OP_SETTABUP, var->u.ind.t, var->u.ind.idx, ex);
      break;
    }
    case VINDEXI: {
      // 目标变量是寄存器表，k就是idx的整数
      // R[A][B] := RK(C)
      codeABRK(fs, OP_SETI, var->u.ind.t, var->u.ind.idx, ex);
      break;
    }
    case VINDEXSTR: {
      // 目标变量是寄存器表，k在常量表中字符串
      // R[A][K[B]:shortstring] := RK(C)
      codeABRK(fs, OP_SETFIELD, var->u.ind.t, var->u.ind.idx, ex);
      break;
    }
    case VINDEXED: {
      // 目标变量是寄存器表，k是寄存器
      // R[A][R[B]] := RK(C)
      codeABRK(fs, OP_SETTABLE, var->u.ind.t, var->u.ind.idx, ex);
      break;
    }
    default: lua_assert(0);  /* invalid var kind to store */
  }
  freeexp(fs, ex);
}


/*
** Emit SELF instruction (convert expression 'e' into 'e:key(e,').
*/
void luaK_self (FuncState *fs, expdesc *e, expdesc *key) {
  int ereg;
  luaK_exp2anyreg(fs, e);
  ereg = e->u.info;  /* register where 'e' was placed */
  freeexp(fs, e);
  e->u.info = fs->freereg;  /* base register for op_self */
  e->k = VNONRELOC;  /* self expression has a fixed register */
  luaK_reserveregs(fs, 2);  /* function and 'self' produced by op_self */
  codeABRK(fs, OP_SELF, e->u.info, ereg, key);
  freeexp(fs, key);
}


/*
** Negate condition 'e' (where 'e' is a comparison).
*/
// 反转条件表达式的逻辑
static void negatecondition (FuncState *fs, expdesc *e) {
  // 根据e->u.info（跳转指令的位置），获取控制跳转的字节码指令
  // 这条指令通常是条件测试指令
  Instruction *pc = getjumpcontrol(fs, e->u.info);
  lua_assert(testTMode(GET_OPCODE(*pc)) && GET_OPCODE(*pc) != OP_TESTSET &&
                                           GET_OPCODE(*pc) != OP_TEST);
  SETARG_k(*pc, (GETARG_k(*pc) ^ 1));
}


/*
** Emit instruction to jump if 'e' is 'cond' (that is, if 'cond'
** is true, code will jump if 'e' is true.) Return jump position.
** Optimize when 'e' is 'not' something, inverting the condition
** and removing the 'not'.
*/
// 根据表达式e和条件cond的值，生成条件+跳转指令，并返回跳转指令的位置
// cond为0，0表示当表达式e为假时跳转
// cond为1，1表示当表达式e为真时跳转
static int jumponcond (FuncState *fs, expdesc *e, int cond) {
  if (e->k == VRELOC) {
    Instruction ie = getinstruction(fs, e);
    if (GET_OPCODE(ie) == OP_NOT) {
      // 移除之前的OP_NOT指令
      removelastinstruction(fs);  /* remove previous OP_NOT */
      return condjump(fs, OP_TEST, GETARG_B(ie), 0, 0, !cond);
    }
    /* else go through */
  }
  // 将表达式的值加载到寄存器中
  discharge2anyreg(fs, e);
  // 释放表达式的资源
  freeexp(fs, e);
  // 生成条件+跳转指令，返回跳转指令的位置
  // cond为0，0表示当表达式e为假时跳转，为真时pc++继续执行
  // cond为1，1表示当表达式e为真时跳转，为假时pc++继续执行
  return condjump(fs, OP_TESTSET, NO_REG, e->u.info, 0, cond);
}


/*
** Emit code to go through if 'e' is true, jump otherwise.
*/
// 当表达式e为真时继续执行当前代码路径，而当e为假时跳转到其他位置 
void luaK_goiftrue (FuncState *fs, expdesc *e) {
  // e为假时跳转的指令下标
  int pc;  /* pc of new jump */
  // 表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VJMP: {  /* condition? */
      // 条件表达式都是反着(与实际代码)？要不这里怎么反着
      // 反转跳转条件，确保在表达式为假时跳转
      negatecondition(fs, e);  /* jump when it is false */
      // 跳转指令下标
      pc = e->u.info;  /* save jump position */
      break;
    }
    case VK: case VKFLT: case VKINT: case VKSTR: case VTRUE: {
      // 说明常量条件表达式，始终为真，无需跳转
      pc = NO_JUMP;  /* always true; do nothing */
      break;
    }
    default: {
      // 对于其他类型的表达式，生成跳转指令，0表示当表达式e为假时跳转
      pc = jumponcond(fs, e, 0);  /* jump when false */
      break;
    }
  }
  // 将新生成的跳转指令添加到e->f（假跳转列表）中，这个时候还不知道跳转到哪里
  luaK_concat(fs, &e->f, pc);  /* insert new jump in false list */
  // 修补真跳转列表，使其跳转到当前位置（即继续执行当前代码路径）
  luaK_patchtohere(fs, e->t);  /* true list jumps to here (to go through) */
  // 上面都处理完了，真跳转列表为空
  e->t = NO_JUMP;
}


/*
** Emit code to go through if 'e' is false, jump otherwise.
*/
void luaK_goiffalse (FuncState *fs, expdesc *e) {
  int pc;  /* pc of new jump */
  luaK_dischargevars(fs, e);
  switch (e->k) {
    case VJMP: {
      pc = e->u.info;  /* already jump if true */
      break;
    }
    case VNIL: case VFALSE: {
      pc = NO_JUMP;  /* always false; do nothing */
      break;
    }
    default: {
      pc = jumponcond(fs, e, 1);  /* jump if true */
      break;
    }
  }
  luaK_concat(fs, &e->t, pc);  /* insert new jump in 't' list */
  luaK_patchtohere(fs, e->f);  /* false list jumps to here (to go through) */
  e->f = NO_JUMP;
}


/*
** Code 'not e', doing constant folding.
*/
// 对表达式e应用逻辑非操作，并尽可能进行常量折叠优化
static void codenot (FuncState *fs, expdesc *e) {
  switch (e->k) {
    case VNIL: case VFALSE: {
      // 如果表达式的值是nil或false，则结果为true
      e->k = VTRUE;  /* true == not nil == not false */
      break;
    }
    case VK: case VKFLT: case VKINT: case VKSTR: case VTRUE: {
      // 如果表达式的值是常量（如字符串、浮点数、整数或布尔值true），则结果为false
      e->k = VFALSE;  /* false == not "x" == not 0.5 == not 1 == not true */
      break;
    }
    case VJMP: {
      // 如果表达式是一个跳转指令，则进行反转
      negatecondition(fs, e);
      break;
    }
    case VRELOC:
    case VNONRELOC: {
      // 如果表达式的值存储在寄存器中，生成一条OP_NOT指令
      // 如果表达式不是VNONRELOC，将表达式的值加载到一个寄存器中，并将表达式标记为不可重定位（VNONRELOC）
      discharge2anyreg(fs, e);
      // 释放表达式的资源
      freeexp(fs, e);
      e->u.info = luaK_codeABC(fs, OP_NOT, 0, e->u.info, 0);
      e->k = VRELOC;
      break;
    }
    default: lua_assert(0);  /* cannot happen */
  }
  /* interchange true and false lists */
  // 交换表达式的真值列表（e->t）和假值列表（e->f），这是因为逻辑非操作会反转条件的真假性
  { int temp = e->f; e->f = e->t; e->t = temp; }
  removevalues(fs, e->f);  /* values are useless when negated */
  removevalues(fs, e->t);
}


/*
** Check whether expression 'e' is a short literal string
*/
// 检查表达式e是否是短字符串字面值
static int isKstr (FuncState *fs, expdesc *e) {
  return (e->k == VK && !hasjumps(e) && e->u.info <= MAXARG_B &&
          ttisshrstring(&fs->f->k[e->u.info]));
}

/*
** Check whether expression 'e' is a literal integer.
*/
// 检查表达式e是否是整数字面量
static int isKint (expdesc *e) {
  return (e->k == VKINT && !hasjumps(e));
}


/*
** Check whether expression 'e' is a literal integer in
** proper range to fit in register C
*/
// 检查表达式e是否是一个适合存储在iABC中C位
static int isCint (expdesc *e) {
  return isKint(e) && (l_castS2U(e->u.ival) <= l_castS2U(MAXARG_C));
}


/*
** Check whether expression 'e' is a literal integer in
** proper range to fit in register sC
*/
static int isSCint (expdesc *e) {
  return isKint(e) && fitsC(e->u.ival);
}


/*
** Check whether expression 'e' is a literal integer or float in
** proper range to fit in a register (sB or sC).
*/
// 检查表达式是否是一个适合存储在sC位中的整数或浮点数
static int isSCnumber (expdesc *e, int *pi, int *isfloat) {
  lua_Integer i;
  if (e->k == VKINT)
    i = e->u.ival;
  else if (e->k == VKFLT && luaV_flttointeger(e->u.nval, &i, F2Ieq))
    // 标记为浮点数
    *isfloat = 1;
  else
    return 0;  /* not a number */
  if (!hasjumps(e) && fitsC(i)) {
    *pi = int2sC(cast_int(i));
    return 1;
  }
  else
    return 0;
}


/*
** Create expression 't[k]'. 't' must have its final result already in a
** register or upvalue. Upvalues can only be indexed by literal strings.
** Keys can be literal strings in the constant table or arbitrary
** values in registers.
*/
// 生成表索引操作t[k]表达式到k
// 将表t和键k组合成一个索引表达式，并根据具体情况选择合适的表达式类型
// （VINDEXUP,VINDEXSTR,VINDEXI,VINDEXED）
void luaK_indexed (FuncState *fs, expdesc *t, expdesc *k) {
  // 如果键是字符串字面量
  if (k->k == VKSTR)
    // 调用str2K将字符串字面量转换为常量表中的索引
    // k存储在了常量表中
    // 既然k是字符串字面量，就将其转换成VK，即Proto::k中的下标
    str2K(fs, k);
  lua_assert(!hasjumps(t) &&
             (t->k == VLOCAL || t->k == VNONRELOC || t->k == VUPVAL));
  if (t->k == VUPVAL && !isKstr(fs, k))  /* upvalue indexed by non 'Kstr'? */
    // 如果表t是上值，并且键k不是字符串字面量，则将上值加载到寄存器中。
    // 这是因为上值不能直接与非字符串键一起使用。
    // 上值不能直接与非字符串键一起使用，因为Lua的虚拟机指令集不支持这种组合
    luaK_exp2anyreg(fs, t);  /* put it in a register */
  if (t->k == VUPVAL) {
    // t是upvalue，k是字符串字面值
    // info记录在Proto::upvalues中的下标
    int temp = t->u.info;  /* upvalue index */
    lua_assert(isKstr(fs, k));
    // 记录上值索引t->u.ind.t和字符串常量索引t->u.ind.idx
    t->u.ind.t = temp;  /* (can't do a direct assignment; values overlap) */
    t->u.ind.idx = k->u.info;  /* literal short string */
    t->k = VINDEXUP;
  }
  else {
    // t不是上值，寄存器位置
    /* register index of the table */
    t->u.ind.t = (t->k == VLOCAL) ? t->u.var.ridx: t->u.info;
    if (isKstr(fs, k)) {
      // t.k/t[str]，k在常量表中
      t->u.ind.idx = k->u.info;  /* literal short string */
      t->k = VINDEXSTR;
    }
    else if (isCint(k)) {
      // t.k/t[int]，k<=l_castS2U(MAXARG_C)
      t->u.ind.idx = cast_int(k->u.ival);  /* int. constant in proper range */
      t->k = VINDEXI;
    }
    else {
      // 寄存器
      t->u.ind.idx = luaK_exp2anyreg(fs, k);  /* register */
      t->k = VINDEXED;
    }
  }
}


/*
** Return false if folding can raise an error.
** Bitwise operations need operands convertible to integers; division
** operations cannot have 0 as divisor.
*/
// 检查操作符是否可以安全地进行常量折叠
static int validop (int op, TValue *v1, TValue *v2) {
  switch (op) {
    case LUA_OPBAND: case LUA_OPBOR: case LUA_OPBXOR:
    case LUA_OPSHL: case LUA_OPSHR: case LUA_OPBNOT: {  /* conversion errors */
      // 对于上面运算操作，确保v1和v2都可以转成整数
      lua_Integer i;
      return (luaV_tointegerns(v1, &i, LUA_FLOORN2I) &&
              luaV_tointegerns(v2, &i, LUA_FLOORN2I));
    }
    case LUA_OPDIV: case LUA_OPIDIV: case LUA_OPMOD:  /* division by 0 */
      // 这些运算符保证被除数不为0
      return (nvalue(v2) != 0);
    default: return 1;  /* everything else is valid */
  }
}


/*
** Try to "constant-fold" an operation; return 1 iff successful.
** (In this case, 'e1' has the final result.)
*/
// 常量折叠
// 尝试在编译阶段对两个常量表达式进行运算，并将结果直接存储到左侧表达式中，从而优化运行时性能
static int constfolding (FuncState *fs, int op, expdesc *e1,
                                        const expdesc *e2) {
  TValue v1, v2, res;
  // 检查e1/e2是否是数子常量，写入到v1/v2中，
  if (!tonumeral(e1, &v1) || !tonumeral(e2, &v2) || !validop(op, &v1, &v2))
    return 0;  /* non-numeric operands or not safe to fold */
  luaO_rawarith(fs->ls->L, op, &v1, &v2, &res);  /* does operation */
  if (ttisinteger(&res)) {
    e1->k = VKINT;
    e1->u.ival = ivalue(&res);
  }
  else {  /* folds neither NaN nor 0.0 (to avoid problems with -0.0) */
    lua_Number n = fltvalue(&res);
    if (luai_numisnan(n) || n == 0)
      return 0;
    e1->k = VKFLT;
    e1->u.nval = n;
  }
  return 1;
}


/*
** Convert a BinOpr to an OpCode  (ORDER OPR - ORDER OP)
*/
// 根据运算符的类型生成相应的指令
// opr当前的二元运算符
// baser基准运算符
// base基础操作码
l_sinline OpCode binopr2op (BinOpr opr, BinOpr baser, OpCode base) {
  lua_assert(baser <= opr &&
            ((baser == OPR_ADD && opr <= OPR_SHR) ||
             (baser == OPR_LT && opr <= OPR_LE)));
  return cast(OpCode, (cast_int(opr) - cast_int(baser)) + cast_int(base));
}


/*
** Convert a UnOpr to an OpCode  (ORDER OPR - ORDER OP)
*/
// 将一元操作符映射为对应的指令
l_sinline OpCode unopr2op (UnOpr opr) {
  return cast(OpCode, (cast_int(opr) - cast_int(OPR_MINUS)) +
                                       cast_int(OP_UNM));
}


/*
** Convert a BinOpr to a tag method  (ORDER OPR - ORDER TM)
*/
l_sinline TMS binopr2TM (BinOpr opr) {
  lua_assert(OPR_ADD <= opr && opr <= OPR_SHR);
  return cast(TMS, (cast_int(opr) - cast_int(OPR_ADD)) + cast_int(TM_ADD));
}


/*
** Emit code for unary expressions that "produce values"
** (everything but 'not').
** Expression to produce final result will be encoded in 'e'.
*/
// 生成一元指令和对应的表达式e
static void codeunexpval (FuncState *fs, OpCode op, expdesc *e, int line) {
  int r = luaK_exp2anyreg(fs, e);  /* opcodes operate only on registers */
  freeexp(fs, e);
  e->u.info = luaK_codeABC(fs, op, 0, r, 0);  /* generate opcode */
  e->k = VRELOC;  /* all those operations are relocatable */
  // 修正：将生成的字节码指令与源代码的行号line关联起来
  luaK_fixline(fs, line);
}


/*
** Emit code for binary expressions that "produce values"
** (everything but logical operators 'and'/'or' and comparison
** operators).
** Expression to produce final result will be encoded in 'e1'.
*/
static void finishbinexpval (FuncState *fs, expdesc *e1, expdesc *e2,
                             OpCode op, int v2, int flip, int line,
                             OpCode mmop, TMS event) {
  int v1 = luaK_exp2anyreg(fs, e1);
  int pc = luaK_codeABCk(fs, op, 0, v1, v2, 0);
  freeexps(fs, e1, e2);
  e1->u.info = pc;
  e1->k = VRELOC;  /* all those operations are relocatable */
  luaK_fixline(fs, line);
  luaK_codeABCk(fs, mmop, v1, v2, event, flip);  /* to call metamethod */
  luaK_fixline(fs, line);
}


/*
** Emit code for binary expressions that "produce values" over
** two registers.
*/
static void codebinexpval (FuncState *fs, BinOpr opr,
                           expdesc *e1, expdesc *e2, int line) {
  OpCode op = binopr2op(opr, OPR_ADD, OP_ADD);
  int v2 = luaK_exp2anyreg(fs, e2);  /* make sure 'e2' is in a register */
  /* 'e1' must be already in a register or it is a constant */
  lua_assert((VNIL <= e1->k && e1->k <= VKSTR) ||
             e1->k == VNONRELOC || e1->k == VRELOC);
  lua_assert(OP_ADD <= op && op <= OP_SHR);
  finishbinexpval(fs, e1, e2, op, v2, 0, line, OP_MMBIN, binopr2TM(opr));
}


/*
** Code binary operators with immediate operands.
*/
static void codebini (FuncState *fs, OpCode op,
                       expdesc *e1, expdesc *e2, int flip, int line,
                       TMS event) {
  int v2 = int2sC(cast_int(e2->u.ival));  /* immediate operand */
  lua_assert(e2->k == VKINT);
  finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINI, event);
}


/*
** Code binary operators with K operand.
*/
static void codebinK (FuncState *fs, BinOpr opr,
                      expdesc *e1, expdesc *e2, int flip, int line) {
  TMS event = binopr2TM(opr);
  int v2 = e2->u.info;  /* K index */
  OpCode op = binopr2op(opr, OPR_ADD, OP_ADDK);
  finishbinexpval(fs, e1, e2, op, v2, flip, line, OP_MMBINK, event);
}


/* Try to code a binary operator negating its second operand.
** For the metamethod, 2nd operand must keep its original value.
*/
static int finishbinexpneg (FuncState *fs, expdesc *e1, expdesc *e2,
                             OpCode op, int line, TMS event) {
  if (!isKint(e2))
    return 0;  /* not an integer constant */
  else {
    lua_Integer i2 = e2->u.ival;
    if (!(fitsC(i2) && fitsC(-i2)))
      return 0;  /* not in the proper range */
    else {  /* operating a small integer constant */
      int v2 = cast_int(i2);
      finishbinexpval(fs, e1, e2, op, int2sC(-v2), 0, line, OP_MMBINI, event);
      /* correct metamethod argument */
      SETARG_B(fs->f->code[fs->pc - 1], int2sC(v2));
      return 1;  /* successfully coded */
    }
  }
}


// 交换表达式
static void swapexps (expdesc *e1, expdesc *e2) {
  expdesc temp = *e1; *e1 = *e2; *e2 = temp;  /* swap 'e1' and 'e2' */
}


/*
** Code binary operators with no constant operand.
*/
static void codebinNoK (FuncState *fs, BinOpr opr,
                        expdesc *e1, expdesc *e2, int flip, int line) {
  if (flip)
    swapexps(e1, e2);  /* back to original order */
  codebinexpval(fs, opr, e1, e2, line);  /* use standard operators */
}


/*
** Code arithmetic operators ('+', '-', ...). If second operand is a
** constant in the proper range, use variant opcodes with K operands.
*/
static void codearith (FuncState *fs, BinOpr opr,
                       expdesc *e1, expdesc *e2, int flip, int line) {
  if (tonumeral(e2, NULL) && luaK_exp2K(fs, e2))  /* K operand? */
    codebinK(fs, opr, e1, e2, flip, line);
  else  /* 'e2' is neither an immediate nor a K operand */
    codebinNoK(fs, opr, e1, e2, flip, line);
}


/*
** Code commutative operators ('+', '*'). If first operand is a
** numeric constant, change order of operands to try to use an
** immediate or K operator.
*/
static void codecommutative (FuncState *fs, BinOpr op,
                             expdesc *e1, expdesc *e2, int line) {
  int flip = 0;
  if (tonumeral(e1, NULL)) {  /* is first operand a numeric constant? */
    swapexps(e1, e2);  /* change order */
    flip = 1;
  }
  if (op == OPR_ADD && isSCint(e2))  /* immediate operand? */
    codebini(fs, OP_ADDI, e1, e2, flip, line, TM_ADD);
  else
    codearith(fs, op, e1, e2, flip, line);
}


/*
** Code bitwise operations; they are all commutative, so the function
** tries to put an integer constant as the 2nd operand (a K operand).
*/
static void codebitwise (FuncState *fs, BinOpr opr,
                         expdesc *e1, expdesc *e2, int line) {
  int flip = 0;
  if (e1->k == VKINT) {
    swapexps(e1, e2);  /* 'e2' will be the constant operand */
    flip = 1;
  }
  if (e2->k == VKINT && luaK_exp2K(fs, e2))  /* K operand? */
    codebinK(fs, opr, e1, e2, flip, line);
  else  /* no constants */
    codebinNoK(fs, opr, e1, e2, flip, line);
}


/*
** Emit code for order comparisons. When using an immediate operand,
** 'isfloat' tells whether the original value was a float.
*/
// ​生成用于顺序比较（<、>、<=、>=）的条件+跳转指令
static void codeorder (FuncState *fs, BinOpr opr, expdesc *e1, expdesc *e2) {
  int r1, r2;
  int im;
  int isfloat = 0;
  OpCode op;
  if (isSCnumber(e2, &im, &isfloat)) {
    // 如果右侧操作数e2是一个简单的数值常量（立即数）
    /* use immediate operand */
    // 将左侧操作数e1加载到寄存器中
    r1 = luaK_exp2anyreg(fs, e1);
    // 立即数im作为右侧操作数
    r2 = im;
    // 返回对应的指令
    op = binopr2op(opr, OPR_LT, OP_LTI);
  }
  else if (isSCnumber(e1, &im, &isfloat)) {
    // 如果左侧操作数e1是一个简单的数值常量（立即数）
    /* transform (A < B) to (B > A) and (A <= B) to (B >= A) */
    // 将右侧操作数e2加载到寄存器中
    r1 = luaK_exp2anyreg(fs, e2);
    // 立即数im作为左侧操作数
    r2 = im;
    // 返回对应的指令
    op = binopr2op(opr, OPR_LT, OP_GTI);
  }
  else {  /* regular case, compare two registers */
    // 两个操作数都不是立即数，将两个操作数分别加载到寄存器中
    r1 = luaK_exp2anyreg(fs, e1);
    r2 = luaK_exp2anyreg(fs, e2);
    // 返回对应的指令
    op = binopr2op(opr, OPR_LT, OP_LT);
  }
  // 释放操作数的资源
  freeexps(fs, e1, e2);
  // 生成跳转指令到表达式中
  e1->u.info = condjump(fs, op, r1, r2, isfloat, 1);
  e1->k = VJMP;
}


/*
** Emit code for equality comparisons ('==', '~=').
** 'e1' was already put as RK by 'luaK_infix'.
*/
// 生成用于相等性比较（==或~=）的字节码指令写入e1
// e1->u.info写入的是跳转指令的下标
static void codeeq (FuncState *fs, BinOpr opr, expdesc *e1, expdesc *e2) {
  // 比较操作的两个操作数所在的寄存器，r2不一定是寄存器
  int r1, r2;
  // 是一个立即数（Immediate Operand），用于存储右侧表达式的值（如果它是简单的整数或浮点数常量）
  int im;
  // 是否是浮点数
  int isfloat = 0;  /* not needed here, but kept for symmetry */
  // 待生成的指令
  OpCode op;
  if (e1->k != VNONRELOC) {
    // 如果e1不是VNONRELOC类型（即它不在寄存器中），则交换e1和e2
    // 这是为了确保左侧表达式始终是寄存器中的值
    lua_assert(e1->k == VK || e1->k == VKINT || e1->k == VKFLT);
    swapexps(e1, e2);
  }
  // 确保e1在寄存器中
  r1 = luaK_exp2anyreg(fs, e1);  /* 1st expression must be in register */
  // 检查右侧表达式是否是简单常量数值
  if (isSCnumber(e2, &im, &isfloat)) {
    op = OP_EQI;
    // 右侧表达式的结果，就是一个简单常量数值
    r2 = im;  /* immediate operand */
  }
  // 检查右侧表达式是否是可以放入常量表
  else if (exp2RK(fs, e2)) {  /* 2nd expression is constant? */
    op = OP_EQK;
    // 常量表中下标
    r2 = e2->u.info;  /* constant index */
  }
  // 见面两者都不是，那么右侧表达式肯定要在寄存器中
  else {
    op = OP_EQ;  /* will compare two registers */
    // 肯定是寄存器下标
    r2 = luaK_exp2anyreg(fs, e2);
  }
  // 尝试释放表达式e1和e2使用的寄存器
  freeexps(fs, e1, e2);
  // 生成指令，最后返回的条件+跳转指令，前一个是条件指令，返回的是跳转指令下标
  // (OPR_EQ == OPR_EQ) = 1，说明当表达式r1~=r2时pc++，跳过 条件失败跳转语句
  // (OPR_NE == OPR_EQ) = 0，说明当表达式r1==r2时pc++，跳过 条件失败跳转语句
  e1->u.info = condjump(fs, op, r1, r2, isfloat, (opr == OPR_EQ));
  e1->k = VJMP;
}


/*
** Apply prefix operation 'op' to expression 'e'.
*/
// 对表达式e应用前缀操作符op
void luaK_prefix (FuncState *fs, UnOpr opr, expdesc *e, int line) {
  // 下面的常量折叠需要这个参数，模拟二元操作的第二个操作数
  static const expdesc ef = {VKINT, {0}, NO_JUMP, NO_JUMP};
  // 表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e);
  switch (opr) {
    case OPR_MINUS: case OPR_BNOT:  /* use 'ef' as fake 2nd operand */
      // 尝试一下常量折叠优化，结果写入e
      if (constfolding(fs, opr + LUA_OPUNM, e, &ef))
        break;
      /* else */ /* FALLTHROUGH */
    case OPR_LEN:
      // 生成一元操作符对应指令表达式到e
      codeunexpval(fs, unopr2op(opr), e, line);
      break;
      // 逻辑非，对表达式e应用逻辑非操作
    case OPR_NOT: codenot(fs, e); break;
    default: lua_assert(0);
  }
}


/*
** Process 1st operand 'v' of binary operation 'op' before reading
** 2nd operand.
*/
// 根据运算符的类型，对第一个操作数进行必要的预处理，以便为解析和计算第二个操作数做好准备
void luaK_infix (FuncState *fs, BinOpr op, expdesc *v) {
  // 将左侧表达式的值加载到寄存器中或者变成可以直接用的形式
  luaK_dischargevars(fs, v);
  switch (op) {
    // 逻辑与
    case OPR_AND: {
      // 只有当v为真时才继续
      luaK_goiftrue(fs, v);  /* go ahead only if 'v' is true */
      break;
    }
    case OPR_OR: {
      luaK_goiffalse(fs, v);  /* go ahead only if 'v' is false */
      break;
    }
    case OPR_CONCAT: {
      luaK_exp2nextreg(fs, v);  /* operand must be on the stack */
      break;
    }
    case OPR_ADD: case OPR_SUB:
    case OPR_MUL: case OPR_DIV: case OPR_IDIV:
    case OPR_MOD: case OPR_POW:
    case OPR_BAND: case OPR_BOR: case OPR_BXOR:
    case OPR_SHL: case OPR_SHR: {
      if (!tonumeral(v, NULL))
        luaK_exp2anyreg(fs, v);
      /* else keep numeral, which may be folded or used as an immediate
         operand */
      break;
    }
    case OPR_EQ: case OPR_NE: {
      if (!tonumeral(v, NULL))
        // 非常量数值，将左侧表达式的值加载到常量表或寄存器中
        exp2RK(fs, v);
      /* else keep numeral, which may be an immediate operand */
      break;
    }
    case OPR_LT: case OPR_LE:
    case OPR_GT: case OPR_GE: {
      int dummy, dummy2;
      // 看看v是否是一个适合存储在sC位中的整数或浮点数
      // 如果是，则保留原样，因为它可能作为立即操作数使用
      if (!isSCnumber(v, &dummy, &dummy2))
        // 将操作数加载到寄存器中
        luaK_exp2anyreg(fs, v);
      /* else keep numeral, which may be an immediate operand */
      break;
    }
    default: lua_assert(0);
  }
}

/*
** Create code for '(e1 .. e2)'.
** For '(e1 .. e2.1 .. e2.2)' (which is '(e1 .. (e2.1 .. e2.2))',
** because concatenation is right associative), merge both CONCATs.
*/
static void codeconcat (FuncState *fs, expdesc *e1, expdesc *e2, int line) {
  Instruction *ie2 = previousinstruction(fs);
  if (GET_OPCODE(*ie2) == OP_CONCAT) {  /* is 'e2' a concatenation? */
    int n = GETARG_B(*ie2);  /* # of elements concatenated in 'e2' */
    lua_assert(e1->u.info + 1 == GETARG_A(*ie2));
    freeexp(fs, e2);
    SETARG_A(*ie2, e1->u.info);  /* correct first element ('e1') */
    SETARG_B(*ie2, n + 1);  /* will concatenate one more element */
  }
  else {  /* 'e2' is not a concatenation */
    luaK_codeABC(fs, OP_CONCAT, e1->u.info, 2, 0);  /* new concat opcode */
    freeexp(fs, e2);
    luaK_fixline(fs, line);
  }
}


/*
** Finalize code for binary operation, after reading 2nd operand.
*/
// 计算二元运算符到表达式中
// opr当前的二元运算符
// e1左侧表达式的描述信息
// e2右侧表达式的描述信息
// line当前行号，用于调试信息
void luaK_posfix (FuncState *fs, BinOpr opr,
                  expdesc *e1, expdesc *e2, int line) {
  // 表达式的值“提取”出来，放到寄存器里或者变成可以直接用的形式，方便后续操作
  luaK_dischargevars(fs, e2);
  // 常量折叠
  if (foldbinop(opr) && constfolding(fs, opr + LUA_OPADD, e1, e2))
    return;  /* done by folding */
  switch (opr) {
    case OPR_AND: {
      lua_assert(e1->t == NO_JUMP);  /* list closed by 'luaK_infix' */
      luaK_concat(fs, &e2->f, e1->f);
      *e1 = *e2;
      break;
    }
    case OPR_OR: {
      lua_assert(e1->f == NO_JUMP);  /* list closed by 'luaK_infix' */
      luaK_concat(fs, &e2->t, e1->t);
      *e1 = *e2;
      break;
    }
    case OPR_CONCAT: {  /* e1 .. e2 */
      luaK_exp2nextreg(fs, e2);
      codeconcat(fs, e1, e2, line);
      break;
    }
    case OPR_ADD: case OPR_MUL: {
      codecommutative(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SUB: {
      if (finishbinexpneg(fs, e1, e2, OP_ADDI, line, TM_SUB))
        break; /* coded as (r1 + -I) */
      /* ELSE */
    }  /* FALLTHROUGH */
    case OPR_DIV: case OPR_IDIV: case OPR_MOD: case OPR_POW: {
      codearith(fs, opr, e1, e2, 0, line);
      break;
    }
    case OPR_BAND: case OPR_BOR: case OPR_BXOR: {
      codebitwise(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SHL: {
      if (isSCint(e1)) {
        swapexps(e1, e2);
        codebini(fs, OP_SHLI, e1, e2, 1, line, TM_SHL);  /* I << r2 */
      }
      else if (finishbinexpneg(fs, e1, e2, OP_SHRI, line, TM_SHL)) {
        /* coded as (r1 >> -I) */;
      }
      else  /* regular case (two registers) */
       codebinexpval(fs, opr, e1, e2, line);
      break;
    }
    case OPR_SHR: {
      if (isSCint(e2))
        codebini(fs, OP_SHRI, e1, e2, 0, line, TM_SHR);  /* r1 >> I */
      else  /* regular case (two registers) */
        codebinexpval(fs, opr, e1, e2, line);
      break;
    }
    // 等于、不等于
    case OPR_EQ: case OPR_NE: {
      // 生成条件+跳转指令，e1跳转，e1->u.info写入的是跳转指令的下标
      codeeq(fs, opr, e1, e2);
      break;
    }
    case OPR_GT: case OPR_GE: {
      /* '(a > b)' <=> '(b < a)';  '(a >= b)' <=> '(b <= a)' */
      // 在数学中，a>b等价于b<a，a>=b等价于b<=a
      // 这种对称性可以用来简化编译器的实现：只需要为<和<=生成字节码，
      // 而>和>=可以通过交换操作数的方式复用相同的逻辑
      swapexps(e1, e2);
      opr = cast(BinOpr, (opr - OPR_GT) + OPR_LT);
    }  /* FALLTHROUGH */
    case OPR_LT: case OPR_LE: {
      // 生成跳转+跳转指令，e1跳转，e1->u.info写入的是跳转指令的下标
      codeorder(fs, opr, e1, e2);
      break;
    }
    default: lua_assert(0);
  }
}


/*
** Change line information associated with current position, by removing
** previous info and adding it again with new line.
*/
// 更新当前字节码位置的行号信息，删除之前的行号信息，并重新添加新的行号
void luaK_fixline (FuncState *fs, int line) {
  removelastlineinfo(fs);
  savelineinfo(fs, fs->f, line);
}


// 根据数组部分和哈希部分的大小，生成或更新OP_NEWTABLE指令，并在必要时生成额外的OP_EXTRAARG指令
void luaK_settablesize (FuncState *fs, int pc, int ra, int asize, int hsize) {
  Instruction *inst = &fs->f->code[pc];
  int rb = (hsize != 0) ? luaO_ceillog2(hsize) + 1 : 0;  /* hash size */
  int extra = asize / (MAXARG_C + 1);  /* higher bits of array size */
  int rc = asize % (MAXARG_C + 1);  /* lower bits of array size */
  int k = (extra > 0);  /* true iff needs extra argument */
  *inst = CREATE_ABCk(OP_NEWTABLE, ra, rb, rc, k);
  *(inst + 1) = CREATE_Ax(OP_EXTRAARG, extra);
}


/*
** Emit a SETLIST instruction.
** 'base' is register that keeps table;
** 'nelems' is #table plus those to be stored now;
** 'tostore' is number of values (in registers 'base + 1',...) to add to
** table (or LUA_MULTRET to add up to stack top).
*/
// 用于生成OP_SETLIST指令
// base表所在的寄存器索引
// nelems表的当前大小加上要存储的数组字段数量
// tostore要写入表中的字段数量（或特殊标记LUA_MULTRET）
void luaK_setlist (FuncState *fs, int base, int nelems, int tostore) {
  lua_assert(tostore != 0 && tostore <= LFIELDS_PER_FLUSH);
  // 表示要写入表中的字段数量未知（例如多返回值）
  if (tostore == LUA_MULTRET)
    // 表示写入所有可用的值
    tostore = 0;
  if (nelems <= MAXARG_C)
    // 如果nelems小于等于MAXARG_C，生成指令即可
    luaK_codeABC(fs, OP_SETLIST, base, tostore, nelems);
  else {
    // 计算超出部分的数量（extra）和剩余部分的数量
    int extra = nelems / (MAXARG_C + 1);
    nelems %= (MAXARG_C + 1);
    // 生成一条带有额外标志位的OP_SETLIST指令
    luaK_codeABCk(fs, OP_SETLIST, base, tostore, nelems, 1);
    // 生成一条额外参数指令，用于处理超出范围的字段数量
    codeextraarg(fs, extra);
  }
  // 释放了所有存储字段值的寄存器，但保留了表所在的寄存器
  fs->freereg = base + 1;  /* free registers with list values */
}


/*
** return the final target of a jump (skipping jumps to jumps)
*/
static int finaltarget (Instruction *code, int i) {
  int count;
  for (count = 0; count < 100; count++) {  /* avoid infinite loops */
    Instruction pc = code[i];
    if (GET_OPCODE(pc) != OP_JMP)
      break;
     else
       i += GETARG_sJ(pc) + 1;
  }
  return i;
}


/*
** Do a final pass over the code of a function, doing small peephole
** optimizations and adjustments.
*/
// 遍历生成的指令，进行优化和调整
void luaK_finish (FuncState *fs) {
  int i;
  Proto *p = fs->f;
  // 遍历当前函数的所有字节码指令
  for (i = 0; i < fs->pc; i++) {
    Instruction *pc = &p->code[i];
    lua_assert(i == 0 || isOT(*(pc - 1)) == isIT(*pc));
    switch (GET_OPCODE(*pc)) {
      case OP_RETURN0: case OP_RETURN1: {
        if (!(fs->needclose || p->is_vararg))
          break;  /* no extra work */
        /* else use OP_RETURN to do the extra work */
        SET_OPCODE(*pc, OP_RETURN);
      }  /* FALLTHROUGH */
      case OP_RETURN: case OP_TAILCALL: {
        if (fs->needclose)
          SETARG_k(*pc, 1);  /* signal that it needs to close */
        if (p->is_vararg)
          SETARG_C(*pc, p->numparams + 1);  /* signal that it is vararg */
        break;
      }
      case OP_JMP: {
        int target = finaltarget(p->code, i);
        fixjump(fs, i, target);
        break;
      }
      default: break;
    }
  }
}
