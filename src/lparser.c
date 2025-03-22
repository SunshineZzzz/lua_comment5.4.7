/*
** $Id: lparser.c $
** Lua Parser
** See Copyright Notice in lua.h
*/

#define lparser_c
#define LUA_CORE

#include "lprefix.h"


#include <limits.h>
#include <string.h>

#include "lua.h"

#include "lcode.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "llex.h"
#include "lmem.h"
#include "lobject.h"
#include "lopcodes.h"
#include "lparser.h"
#include "lstate.h"
#include "lstring.h"
#include "ltable.h"



/* maximum number of local variables per function (must be smaller
   than 250, due to the bytecode format) */
// 局部变量的最大数量200限制
#define MAXVARS		200


// 函数调用或者可变参数允许多返回值
#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
*/
// 就是lua语法定义中的block，块信息
typedef struct BlockCnt {
  // 上面的block
  struct BlockCnt *previous;  /* chain */
  // 当前语句块中，第一个标签语句在全局已声明的标签中的位置
  int firstlabel;  /* index of first label in this block */
  // 当前语句块中，第一个未匹配标签语句在全局未匹配的标签中的位置
  int firstgoto;  /* index of first pending goto in this block */
  // 在进入本block的时，外面活跃的局部变量的数量
  lu_byte nactvar;  /* # active locals outside the block */
  // 将目标块的upval标记为1，表示该块存在某些变量是其它块的upvalues
  // 该快关闭时候，需要处理
  lu_byte upval;  /* true if some variable in the block is an upvalue */
  // 
  lu_byte isloop;  /* true if 'block' is a loop */
  // 
  lu_byte insidetbc;  /* true if inside the scope of a to-be-closed var. */
} BlockCnt;



/*
** prototypes for recursive non-terminal functions
*/
static void statement (LexState *ls);
static void expr (LexState *ls, expdesc *v);


static l_noret error_expected (LexState *ls, int token) {
  luaX_syntaxerror(ls,
      luaO_pushfstring(ls->L, "%s expected", luaX_token2str(ls, token)));
}


static l_noret errorlimit (FuncState *fs, int limit, const char *what) {
  lua_State *L = fs->ls->L;
  const char *msg;
  int line = fs->f->linedefined;
  const char *where = (line == 0)
                      ? "main function"
                      : luaO_pushfstring(L, "function at line %d", line);
  msg = luaO_pushfstring(L, "too many %s (limit is %d) in %s",
                             what, limit, where);
  luaX_syntaxerror(fs->ls, msg);
}


// 检查限制，超过限制就报错
static void checklimit (FuncState *fs, int v, int l, const char *what) {
  if (v > l) errorlimit(fs, l, what);
}


/*
** Test whether next token is 'c'; if so, skip it.
*/
// 下一个token是否是c
static int testnext (LexState *ls, int c) {
  if (ls->t.token == c) {
    luaX_next(ls);
    return 1;
  }
  else return 0;
}


/*
** Check that next token is 'c'.
*/
// 当前token不是c，就抛出异常
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}


/*
** Check that next token is 'c' and skip it.
*/
// 检测当前token是否是c，并且下一个
static void checknext (LexState *ls, int c) {
  check(ls, c);
  luaX_next(ls);
}


#define check_condition(ls,c,msg)	{ if (!(c)) luaX_syntaxerror(ls, msg); }


/*
** Check that next token is 'what' and skip it. In case of error,
** raise an error that the expected 'what' should match a 'who'
** in line 'where' (if that is not the current line).
*/
// what表示期望的词法单元类型
// who表示与what相关的另一个词法单元类型，通常用于描述需要匹配的上下文。
// where表示who所在的行号
static void check_match (LexState *ls, int what, int who, int where) {
  if (l_unlikely(!testnext(ls, what))) {
    if (where == ls->linenumber)  /* all in the same line? */
      error_expected(ls, what);  /* do not need a complex message */
    else {
      luaX_syntaxerror(ls, luaO_pushfstring(ls->L,
             "%s expected (to close %s at line %d)",
              luaX_token2str(ls, what), luaX_token2str(ls, who), where));
    }
  }
}


// 当前token是否是TK_NAME，不是就会抛出异常，返回TK_NAME对象，下一个token
static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


// 初始化表达式
static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}


// 用内部字符串初始化表达式e
static void codestring (expdesc *e, TString *s) {
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->u.strval = s;
}


// 将标识符（变量名）转换为字符串常量，并存储到表达式描述符中
static void codename (LexState *ls, expdesc *e) {
  codestring(e, str_checkname(ls));
}


/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
// 将新声明的局部变量注册到当前函数的调试信息中
static int registerlocalvar (LexState *ls, FuncState *fs, TString *varname) {
  Proto *f = fs->f;
  int oldsize = f->sizelocvars;
  luaM_growvector(ls->L, f->locvars, fs->ndebugvars, f->sizelocvars,
                  LocVar, SHRT_MAX, "local variables");
  while (oldsize < f->sizelocvars)
    f->locvars[oldsize++].varname = NULL;
  f->locvars[fs->ndebugvars].varname = varname;
  f->locvars[fs->ndebugvars].startpc = fs->pc;
  luaC_objbarrier(ls->L, f, varname);
  return fs->ndebugvars++;
}


/*
** Create a new local variable with the given 'name'. Return its index
** in the function.
*/
// 用给定名称注册一个本地变量，返回下标
static int new_localvar (LexState *ls, TString *name) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Dyndata *dyd = ls->dyd;
  Vardesc *var;
  checklimit(fs, dyd->actvar.n + 1 - fs->firstlocal,
                 MAXVARS, "local variables");
  luaM_growvector(L, dyd->actvar.arr, dyd->actvar.n + 1,
                  dyd->actvar.size, Vardesc, USHRT_MAX, "local variables");
  var = &dyd->actvar.arr[dyd->actvar.n++];
  var->vd.kind = VDKREG;  /* default */
  var->vd.name = name;
  return dyd->actvar.n - 1 - fs->firstlocal;
}

#define new_localvarliteral(ls,v) \
    new_localvar(ls,  \
      luaX_newstring(ls, "" v, (sizeof(v)/sizeof(char)) - 1));



/*
** Return the "variable description" (Vardesc) of a given variable.
** (Unless noted otherwise, all variables are referred to by their
** compiler indices.)
*/
// 获取local变量描述
static Vardesc *getlocalvardesc (FuncState *fs, int vidx) {
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}


/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
// 根据给定的local变量个数，返回对应的寄存器层级(就是下一个可以使用的寄存器层级)
static int reglevel (FuncState *fs, int nvar) {
  // 第一个下标没在用哦
  while (nvar-- > 0) {
    Vardesc *vd = getlocalvardesc(fs, nvar);  /* get previous variable */
    if (vd->vd.kind != RDKCTC)  /* is in a register? */
      return vd->vd.ridx + 1;
  }
  return 0;  /* no variables in registers */
}


/*
** Return the number of variables in the register stack for the given
** function.
*/
// 返回当前函数中所有活动变量所占用的寄存器范围（即寄存器层级）
// 其实也就是：返回给定函数的寄存器堆栈中的变量数
int luaY_nvarstack (FuncState *fs) {
  return reglevel(fs, fs->nactvar);
}


/*
** Get the debug-information entry for current variable 'vidx'.
*/
// 根据vidx获取当前变量的调试信息
static LocVar *localdebuginfo (FuncState *fs, int vidx) {
  Vardesc *vd = getlocalvardesc(fs,  vidx);
  if (vd->vd.kind == RDKCTC)
    return NULL;  /* no debug info. for constants */
  else {
    int idx = vd->vd.pidx;
    lua_assert(idx < fs->ndebugvars);
    return &fs->f->locvars[idx];
  }
}


/*
** Create an expression representing variable 'vidx'
*/
// 初始化一个local变量信息
static void init_var (FuncState *fs, expdesc *e, int vidx) {
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->u.var.vidx = vidx;
  e->u.var.ridx = getlocalvardesc(fs, vidx)->vd.ridx;
}


/*
** Raises an error if variable described by 'e' is read only
*/
// 如果是只读变量，抛出错误
static void check_readonly (LexState *ls, expdesc *e) {
  FuncState *fs = ls->fs;
  TString *varname = NULL;  /* to be set if variable is const */
  switch (e->k) {
    case VCONST: {
      varname = ls->dyd->actvar.arr[e->u.info].vd.name;
      break;
    }
    case VLOCAL: {
      Vardesc *vardesc = getlocalvardesc(fs, e->u.var.vidx);
      if (vardesc->vd.kind != VDKREG)  /* not a regular variable? */
        varname = vardesc->vd.name;
      break;
    }
    case VUPVAL: {
      Upvaldesc *up = &fs->f->upvalues[e->u.info];
      if (up->kind != VDKREG)
        varname = up->name;
      break;
    }
    default:
      return;  /* other cases cannot be read-only */
  }
  if (varname) {
    const char *msg = luaO_pushfstring(ls->L,
       "attempt to assign to const variable '%s'", getstr(varname));
    luaK_semerror(ls, msg);  /* error */
  }
}


/*
** Start the scope for the last 'nvars' created variables.
*/
// 主要目的是分配寄存器索引
// 将新声明的局部变量注册到当前作用域，并为其分配寄存器索引
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++) {
    // 为新变量分配一个唯一的索引
    int vidx = fs->nactvar++;
    // 获取指定索引的局部变量描述符
    Vardesc *var = getlocalvardesc(fs, vidx);
    // 每次分配后，递增reglevel，确保下一个变量使用下一个寄存器
    var->vd.ridx = reglevel++;
    // 将变量的名称注册到符号表中，并返回其在符号表中的索引
    var->vd.pidx = registerlocalvar(ls, fs, var->vd.name);
  }
}


/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
// 关闭从当前作用域到指定层级tolevel作用域的所有变量，并且更新调试信息
static void removevars (FuncState *fs, int tolevel) {
  // 总共活跃变量个数 -= (当前函数的活跃变量个数 - 当前作用域(block)要关闭了，这里是将要恢复到作用域的活跃变量个数)
  // 目标层级活跃变量个数
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  // 循环设置调试信息endpc
  while (fs->nactvar > tolevel) {
    LocVar *var = localdebuginfo(fs, --fs->nactvar);
    if (var)  /* does it have debug information? */
      var->endpc = fs->pc;
  }
}


/*
** Search the upvalues of the function 'fs' for one
** with the given 'name'.
*/
// 查看当前函数层级上值中已有该NAME，有的话则返回对应位置，找不到返回-1
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}


// 分配upvalues，并且返回第一个Upvaldesc对象
static Upvaldesc *allocupvalue (FuncState *fs) {
  Proto *f = fs->f;
  int oldsize = f->sizeupvalues;
  checklimit(fs, fs->nups + 1, MAXUPVAL, "upvalues");
  luaM_growvector(fs->ls->L, f->upvalues, fs->nups, f->sizeupvalues,
                  Upvaldesc, MAXUPVAL, "upvalues");
  while (oldsize < f->sizeupvalues)
    f->upvalues[oldsize++].name = NULL;
  return &f->upvalues[fs->nups++];
}


// 将局部变量或外层函数的上值提升为当前函数的上值，并将其记录到函数的上值描述符列表中
static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  // 分配一个新的上值描述符
  Upvaldesc *up = allocupvalue(fs);
  FuncState *prev = fs->prev;
  if (v->k == VLOCAL) {
    // 是局部变量
    // 该上值来源于栈中的局部变量
    up->instack = 1;
    // 记录寄存器下标
    up->idx = v->u.var.ridx;
    up->kind = getlocalvardesc(prev, v->u.var.vidx)->vd.kind;
    lua_assert(eqstr(name, getlocalvardesc(prev, v->u.var.vidx)->vd.name));
  }
  else {
    // 是外层函数的上值
    // 该上值来源于外层函数的上值
    up->instack = 0;
    // info记录在Proto::upvalues中的下标
    up->idx = cast_byte(v->u.info);
    up->kind = prev->f->upvalues[v->u.info].kind;
    lua_assert(eqstr(name, prev->f->upvalues[v->u.info].name));
  }
  up->name = name;
  luaC_objbarrier(fs->ls->L, fs->f, name);
  return fs->nups - 1;
}


/*
** Look for an active local variable with the name 'n' in the
** function 'fs'. If found, initialize 'var' with it and return
** its expression kind; otherwise return -1.
*/
// 查看当前函数层级是否已有该NAME，有的话则返回对应位置，找不到返回-1
static int searchvar (FuncState *fs, TString *n, expdesc *var) {
  int i;
  for (i = cast_int(fs->nactvar) - 1; i >= 0; i--) {
    Vardesc *vd = getlocalvardesc(fs, i);
    if (eqstr(n, vd->vd.name)) {  /* found? */
      if (vd->vd.kind == RDKCTC)  /* compile-time constant? */
        init_exp(var, VCONST, fs->firstlocal + i);
      else  /* real variable */
        init_var(fs, var, i);
      return var->k;
    }
  }
  return -1;  /* not found */
}


/*
** Mark block where variable at given level was defined
** (to emit close instructions later).
*/
// 标记变量定义所在的块（block），以便在后续生成关闭指令时能够正确处理上值（upvalue）
// fs当前函数的状态信息
// level要标记的变量所在的作用域层级，
// fs->ls->dyd->actvar.arr[fs->firstlocal + vidx]就是这里的vidx
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  // 遍历块链，找到定义变量的块
  // bl->nactvar在进入本block的时，外面活跃的局部变量的数量
  // bl->nactvar > level表示当前块的外部活跃局部变量数量大于目标变量的层级
  while (bl->nactvar > level)
    bl = bl->previous;
  // 将目标块的upval标记为1，表示该块存在某些变量是其它块的upvalues
  bl->upval = 1;
  // level对应的变量所在的函数needclose标记为1，
  // 函数结束时，编译器生成关闭指令，确保变量的生命周期得到正确管理
  fs->needclose = 1;
}


/*
** Mark that current block has a to-be-closed variable.
*/
static void marktobeclosed (FuncState *fs) {
  BlockCnt *bl = fs->bl;
  bl->upval = 1;
  bl->insidetbc = 1;
  fs->needclose = 1;
}


/*
** Find a variable with the given name 'n'. If it is an upvalue, add
** this upvalue into all intermediate functions. If it is a global, set
** 'var' as 'void' as a flag.
*/
//  在函数层级fs中查找名为n的变量，填充var，base为1表示当前函数层级
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL)  /* no more levels? */
    // 如果当前函数层级为NULL，则说明是最外层了，即脚本本身，那么该NAME一定是全局，调用init_exp()初始化VVOID并返回
    init_exp(var, VVOID, 0);  /* default is global */
  else {
    // 函数层级不为NULL，则调用searchvar()查看当前层级是否已有该NAME，有的话则返回对应位置，找不到返回-1
    int v = searchvar(fs, n, var);  /* look up locals at current level */
    if (v >= 0) {  /* found? */
      // 找到了并且不是第一层函数，说白了就是在上层函数中找到了，定义为上值
      if (v == VLOCAL && !base)
        // 标记为上值
        markupval(fs, var->u.var.vidx);  /* local will be used as an upval */
    }
    else {  /* not found as local at current level; try upvalues */
      // 找不到，则在上值中查找
      int idx = searchupvalue(fs, n);  /* try existing upvalues */
      if (idx < 0) {  /* not found? */
        // 上值都找不到，只能去上层函数找了，注意这里的base传递为0
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
        if (var->k == VLOCAL || var->k == VUPVAL)  /* local or upvalue? */
          // 如果是上层函数中的局部变量或者上值，就加入到自身的上值中
          idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
        else  /* it is a global or a constant */
          return;  /* don't need to do anything at this level */
      }
      // 找到置为上值返回
      init_exp(var, VUPVAL, idx);  /* new or old upvalue */
    }
  }
}


/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
// 查找变量，从当前函数层级local查找，找不到当前层级upvalue查找，
// 在上一个函数层级查找local查找，找不到在上一个函数层级upvalue查找，
// 最终会在全局查找
// 注意：如果在上一个函数层级找到，会加入到自身upvalue中
static void singlevar (LexState *ls, expdesc *var) {
  // 拿到变量名称
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  // 在当前函数层级查找
  singlevaraux(fs, varname, var, 1);
  if (var->k == VVOID) {  /* global name? */
    // 说明要到全局变量里面找_ENV
    // var是找到上值的_ENV表达式描述
    // key是该变量名称的表达式描述
    expdesc key;
    // 开找_ENV
    singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
    // 肯定可以找到，_ENV是每个函数第一个上值
    lua_assert(var->k != VVOID);  /* this one must exist */
    // 确保_ENV的结果存储在寄存器或上值中
    // 说不定有人修改了Closure的环境变量，所以要确保是寄存器或者上值
    luaK_exp2anyregup(fs, var);  /* but could be a constant */
    // 生成key字符串字面量类型表达式
    codestring(&key, varname);  /* key is variable name */
    // 创建表达式t[k]到var中
    luaK_indexed(fs, var, &key);  /* env[varname] */
  }
}


/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
// 赋值，将表达式列表e的结果数量从nexps个表达式调整为nvars个值
// 会生成或者调整已有的指令
// nvars左侧变量的数量
// nexps右侧表达式的数量
// e右侧最后一个表达式的描述
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  // 左侧变量数量与右侧表达式数量之间的差值，需要额外补充
  int needed = nvars - nexps;  /* extra values needed */
  // 检查最后一个表达式是否是多重返回值
  if (hasmultret(e->k)) {  /* last expression has multiple returns? */
    // 最后一个右侧表达式是多返回值，则调整其返回值数量以匹配需求
    // +1，排除最后一个表达式本身 
    int extra = needed + 1;  /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    // 确保多返回值表达式提供所需的额外值
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
  }
  else {
    if (e->k != VVOID)  /* at least one expression? */
      // 右侧最后一个表达式也需要确保在寄存器中
      luaK_exp2nextreg(fs, e);  /* close last expression */
    if (needed > 0)  /* missing values? */
      // 创建OP_LOADNIL指令，fs->freereg当前可用寄存器下标，needed额外需要写nil个数
      luaK_nil(fs, fs->freereg, needed);  /* complete with nils */
  }
  if (needed > 0)
    // 1.最后一个表达式是多返回值，肯定会调用OP_VCALL指令或者OP_VVARARG指令参数，所以同样需要预留寄存器
    // 2.上面OP_LOADNIL创建的nil个数，所以这里也需要把寄存器预留出来
    luaK_reserveregs(fs, needed);  /* registers for extra values */
  else  /* adding 'needed' is actually a subtraction */
    fs->freereg += needed;  /* remove extra values */
}


// 进入一个新的block 
#define enterlevel(ls)	luaE_incCstack(ls->L)


// 离开block
#define leavelevel(ls) ((ls)->L->nCcalls--)


/*
** Generates an error that a goto jumps into the scope of some
** local variable.
*/
static l_noret jumpscopeerror (LexState *ls, Labeldesc *gt) {
  const char *varname = getstr(getlocalvardesc(ls->fs, gt->nactvar)->vd.name);
  const char *msg = "<goto %s> at line %d jumps into the scope of local '%s'";
  msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line, varname);
  luaK_semerror(ls, msg);  /* raise the error */
}


/*
** Solves the goto at index 'g' to given 'label' and removes it
** from the list of pending gotos.
** If it jumps into the scope of some variable, raises an error.
*/
// g待匹配的标签在ls->dyd->gt列表中的索引
// label就是上面要匹配的标签
static void solvegoto (LexState *ls, int g, Labeldesc *label) {
  int i;
  // 等待匹配的标签信息数组
  Labellist *gl = &ls->dyd->gt;  /* list of gotos */
  // 取出来未匹配的标签
  Labeldesc *gt = &gl->arr[g];  /* goto to be resolved */
  lua_assert(eqstr(gt->name, label->name));
  // 检查未匹配的标签(goto/break)是否尝试进入其他作用域
  if (l_unlikely(gt->nactvar < label->nactvar))  /* enter some scope? */
    jumpscopeerror(ls, gt);
  // 将待匹配标签的跳转指令修补为目标标签的指令地址
  luaK_patchlist(ls->fs, gt->pc, label->pc);
  // 使用循环将后续未匹配标签向前移动一位，覆盖掉已匹配的标签
  for (i = g; i < gl->n - 1; i++)  /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** Search for an active label with the given name.
*/
// 在已经声明的列表查找标签
static Labeldesc *findlabel (LexState *ls, TString *name) {
  int i;
  Dyndata *dyd = ls->dyd;
  /* check labels in current function for a match */
  for (i = ls->fs->firstlabel; i < dyd->label.n; i++) {
    Labeldesc *lb = &dyd->label.arr[i];
    if (eqstr(lb->name, name))  /* correct label? */
      return lb;
  }
  return NULL;  /* label not found */
}


/*
** Adds a new label/goto in the corresponding list.
*/
// 在对应列表中添加label或者goto
static int newlabelentry (LexState *ls, Labellist *l, TString *name,
                          int line, int pc) {
  int n = l->n;
  luaM_growvector(ls->L, l->arr, n, l->size,
                  Labeldesc, SHRT_MAX, "labels/gotos");
  l->arr[n].name = name;
  l->arr[n].line = line;
  l->arr[n].nactvar = ls->fs->nactvar;
  l->arr[n].close = 0;
  l->arr[n].pc = pc;
  l->n = n + 1;
  return n;
}


// 在对应未匹配列表中添加label或者goto
static int newgotoentry (LexState *ls, TString *name, int line, int pc) {
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}


/*
** Solves forward jumps. Check whether new label 'lb' matches any
** pending gotos in current block and solves them. Return true
** if any of the gotos need to close upvalues.
*/
// 处理前向跳转
static int solvegotos (LexState *ls, Labeldesc *lb) {
  Labellist *gl = &ls->dyd->gt;
  int i = ls->fs->bl->firstgoto;
  // 是否关闭上值
  int needsclose = 0;
  while (i < gl->n) {
    if (eqstr(gl->arr[i].name, lb->name)) {
      needsclose |= gl->arr[i].close;
      solvegoto(ls, i, lb);  /* will remove 'i' from the list */
    }
    else
      i++;
  }
  return needsclose;
}


/*
** Create a new label with the given 'name' at the given 'line'.
** 'last' tells whether label is the last non-op statement in its
** block. Solves all pending gotos to this new label and adds
** a close instruction if necessary.
** Returns true iff it added a close instruction.
*/
// 创建指定名称的新标签
// ls词法状态机
// name标签名称
// line标签所在行号
// last参数指示该标签是否是其所在块中的最后一个非操作语句，就是label后面是不是就是块结束，
// labelh和;被认为是non-op statement
static int createlabel (LexState *ls, TString *name, int line,
                        int last) {
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  // 添加到已经生命的标签数组中
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last) {  /* label is last no-op statement in the block? */
    // 标签是块中的最后一个非操作语句
    // 更新标签的活动变量数量（nactvar），以确保局部变量的作用域正确
    /* assume that locals are already out of scope */
    // newlabelentry这个调用已经置为ls->fs->nactvar，这里发现后面就是块结束，就置为
    // 进入这个块之前的本地变量的个数
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
  // 解决所有待处理的goto
  if (solvegotos(ls, &ll->arr[l])) {  /* need close? */
    luaK_codeABC(fs, OP_CLOSE, luaY_nvarstack(fs), 0, 0);
    return 1;
  }
  return 0;
}


/*
** Adjust pending gotos to outer level of a block.
*/
// 将待匹配的标签调整到块的外部级别
static void movegotosout (FuncState *fs, BlockCnt *bl) {
  int i;
  Labellist *gl = &fs->ls->dyd->gt;
  /* correct pending gotos to current block */
  for (i = bl->firstgoto; i < gl->n; i++) {  /* for each pending goto */
    Labeldesc *gt = &gl->arr[i];
    /* leaving a variable scope? */
    // 
    if (reglevel(fs, gt->nactvar) > reglevel(fs, bl->nactvar))
      gt->close |= bl->upval;  /* jump may need a close */
    gt->nactvar = bl->nactvar;  /* update goto level */
  }
}


// 进入block，初始化信息
static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  // 当前可用空闲位置
  bl->firstlabel = fs->ls->dyd->label.n;
  // 当前可用空闲位置
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  bl->insidetbc = (fs->bl != NULL && fs->bl->insidetbc);
  // 链接起来
  bl->previous = fs->bl;
  // 更新到新的block
  fs->bl = bl;
  lua_assert(fs->freereg == luaY_nvarstack(fs));
}


/*
** generates an error for an undefined 'goto'.
*/
// 当编译器检测到一个未匹配的goto或者非法的break时，生成相应的错误消息并终止编译过程
static l_noret undefgoto (LexState *ls, Labeldesc *gt) {
  const char *msg;
  if (eqstr(gt->name, luaS_newliteral(ls->L, "break"))) {
    msg = "break outside loop at line %d";
    msg = luaO_pushfstring(ls->L, msg, gt->line);
  }
  else {
    msg = "no visible label '%s' for <goto> at line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(gt->name), gt->line);
  }
  luaK_semerror(ls, msg);
}


// 离开block
static void leaveblock (FuncState *fs) {
  // 当前block信息
  BlockCnt *bl = fs->bl;
  // 词法状态机
  LexState *ls = fs->ls;
  // 标志是否需要生成OP_CLOSE指令
  int hasclose = 0;
  // 计算当前块外的寄存器层级，以便在退出块时恢复自由寄存器计数器（freereg）
  // 其实就是下一个可用的寄存器层级
  int stklevel = reglevel(fs, bl->nactvar);  /* level outside the block */
  // 删除当前作用域的局部变量到上一个作用域的变量，并且会更新调试信息(endpc)
  removevars(fs, bl->nactvar);  /* remove block locals */
  lua_assert(bl->nactvar == fs->nactvar);  /* back to level on entry */
  if (bl->isloop)  /* has to fix pending breaks? */
    hasclose = createlabel(ls, luaS_newliteral(ls->L, "break"), 0, 0);
  if (!hasclose && bl->previous && bl->upval)  /* still need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  // 恢复空闲的栈下标
  fs->freereg = stklevel;  /* free registers */
  // 要关闭作用域的lable都移除
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
  // 将当前块切换到父块（bl->previous），以便继续编译嵌套块或外部代码 
  fs->bl = bl->previous;  /* current block now is previous one */
  // 是否有嵌套块
  if (bl->previous)  /* was it a nested block? */
    // 当前作用域中存在未匹配的标签，没在当前作用域找到，所以需要将这些未匹配的标签移动到上一级作用域继续查找标签
    movegotosout(fs, bl);  /* update pending gotos to enclosing block */
  else {
    // 已经最外层的作用域，bl->firstgoto < ls->dyd->gt.n，说明当前代码块中仍有未匹配的标签
    // 不可能存在大于，最多等于，等于就说明没有未匹配的标签
    if (bl->firstgoto < ls->dyd->gt.n)  /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
  }
}


/*
** adds a new prototype into list of prototypes
*/
// 创建新的函数原型，添加到函数原型链中
static Proto *addprototype (LexState *ls) {
  Proto *clp;
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;  /* prototype of current function */
  if (fs->np >= f->sizep) {
    int oldsize = f->sizep;
    luaM_growvector(L, f->p, fs->np, f->sizep, Proto *, MAXARG_Bx, "functions");
    while (oldsize < f->sizep)
      f->p[oldsize++] = NULL;
  }
  f->p[fs->np++] = clp = luaF_newproto(L);
  luaC_objbarrier(L, f, clp);
  return clp;
}


/*
** codes instruction to create new closure in parent function.
** The OP_CLOSURE instruction uses the last available register,
** so that, if it invokes the GC, the GC knows which registers
** are in use at that time.

*/
// 用于生成创建闭包的字节码指令
// 它的主要目的是为闭包分配寄存器，并确保GC能够正确识别哪些寄存器正在使用
static void codeclosure (LexState *ls, expdesc *v) {
  // 拿到父函数
  FuncState *fs = ls->fs->prev;
  // A为0，下面就会调整
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  // 将闭包的结果固定在当前可用的最后一个寄存器中
  // 这是为了确保GC能够正确识别哪些寄存器正在使用
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


// 打开函数，初始化fs
// fs新的，bl新的
static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  Proto *f = fs->f;
  // 链接起来
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  // 切换到现在函数
  ls->fs = fs;
  fs->pc = 0;
  fs->previousline = f->linedefined;
  fs->iwthabs = 0;
  fs->lasttarget = 0;
  fs->freereg = 0;
  fs->nk = 0;
  fs->nabslineinfo = 0;
  fs->np = 0;
  fs->nups = 0;
  fs->ndebugvars = 0;
  fs->nactvar = 0;
  fs->needclose = 0;
  // 当前可用空闲位置
  fs->firstlocal = ls->dyd->actvar.n;
  // 当前可用空闲位置
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = NULL;
  f->source = ls->source;
  luaC_objbarrier(ls->L, f, f->source);
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  enterblock(fs, bl, 0);
}


// 函数编译完成了，关闭函数
static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  // 生成一个最终返回指令，猜测是兜底用的，毕竟有的没有显示写return
  // 参数luaY_nvarstack(fs)表示返回值的起始寄存器索引
  // 参数0表示返回零个值
  luaK_ret(fs, luaY_nvarstack(fs), 0);  /* final return */
  // 离开block
  leaveblock(fs);
  lua_assert(fs->bl == NULL);
  // 遍历生成的指令，进行优化和调
  luaK_finish(fs);
  luaM_shrinkvector(L, f->code, f->sizecode, fs->pc, Instruction);
  luaM_shrinkvector(L, f->lineinfo, f->sizelineinfo, fs->pc, ls_byte);
  luaM_shrinkvector(L, f->abslineinfo, f->sizeabslineinfo,
                       fs->nabslineinfo, AbsLineInfo);
  luaM_shrinkvector(L, f->k, f->sizek, fs->nk, TValue);
  luaM_shrinkvector(L, f->p, f->sizep, fs->np, Proto *);
  luaM_shrinkvector(L, f->locvars, f->sizelocvars, fs->ndebugvars, LocVar);
  luaM_shrinkvector(L, f->upvalues, f->sizeupvalues, fs->nups, Upvaldesc);
  ls->fs = fs->prev;
  luaC_checkGC(L);
}



/*============================================================*/
/* GRAMMAR RULES */
/*============================================================*/


/*
** check whether current token is in the follow set of a block.
** 'until' closes syntactical blocks, but do not close scope,
** so it is handled in separate.
*/
// 当前token是否是block结束标记，对于TK_UNTIL单独进行处理，因为
// 并不关闭作用域
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
      return 1;
    case TK_UNTIL: return withuntil;
    default: return 0;
  }
}


// 解析statlist
/* statlist -> { stat [';'] } */
// 
// chunk :: = block
// block :: = { stat }[retstat]
// retstat ::= return [explist] [‘;’]
static void statlist (LexState *ls) {
  /* statlist -> { stat [';'] } */
  // 当前token只要不是block结束标记，就继续解析，直到遇到block结束
  while (!block_follow(ls, 1)) {
    if (ls->t.token == TK_RETURN) {
      statement(ls);
      return;  /* 'return' must be last statement */
    }
    statement(ls);
  }
}


static void fieldsel (LexState *ls, expdesc *v) {
  /* fieldsel -> ['.' | ':'] NAME */
  FuncState *fs = ls->fs;
  expdesc key;
  luaK_exp2anyregup(fs, v);
  luaX_next(ls);  /* skip the dot or colon */
  codename(ls, &key);
  luaK_indexed(fs, v, &key);
}


// 解析
// index -> '[' expr ']'
static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  // 确保表达式的结果存储在寄存器中或是一个常量
  // 为了处理复杂的kye吧？比如：t[a and b or c]
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


// 管理表的构造过程
typedef struct ConsControl {
  // 在表构造过程中，数组部分都会被解析为一个expdesc结构体
  // 用于临时存储当前处理的项
  expdesc v;  /* last list item read */
  // 表的表达式描述
  expdesc *t;  /* table descriptor */
  // 哈希部分的字段数量
  int nh;  /* total number of 'record' elements */
  // 数组部分的字段数量
  int na;  /* number of array elements already stored */
  // 待存储的字段数量
  int tostore;  /* number of array elements pending to be stored */
} ConsControl;


// 解析
// recfield -> (NAME | '['exp']') = exp
static void recfield (LexState *ls, ConsControl *cc) {
  /* recfield -> (NAME | '['exp']') = exp */
  FuncState *fs = ls->fs;
  // 当前空闲寄存器的起始位置
  int reg = ls->fs->freereg;
  // 表的目标寄存器，键的表达式描述信息，值的表达式描述信息
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME) {
    // 解析NAME
    checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    // 将标识符转换为键的表达式
    codename(ls, &key);
  }
  else  /* ls->t.token == '[' */
    // 解析[exp]形式的键
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  // 生成表索引操作tab[key]表达式到tab
  luaK_indexed(fs, &tab, &key);
  // 解析 = exp
  expr(ls, &val);
  // val写入到tab
  luaK_storevar(fs, &tab, &val);
  // 该释放释放
  fs->freereg = reg;  /* free registers */
}


// 将当前解析到的表达式值添加到表构造器的数组部分，并在必要时刷新缓冲区以生成字节码
static void closelistfield (FuncState *fs, ConsControl *cc) {
  // 表示没有有效的列表字段，直接返回
  if (cc->v.k == VVOID) return;  /* there is no list item */
  // 确保在寄存器中
  luaK_exp2nextreg(fs, &cc->v);
  // 设置为 VVOID，表示当前表达式值已经被处理
  cc->v.k = VVOID;
  // 缓冲区已满
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    // 
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0;  /* no more items pending */
  }
}


// 将数组字段缓冲区中剩余字段加入表中
static void lastlistfield (FuncState *fs, ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    // 如果当前字段是一个多返回值表达式
    // 
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    // 不是多返回值表达式
    if (cc->v.k != VVOID)
      // 形如这种：{1,2,a=1,b=2,3}，3在这里应该需要写入寄存器中
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}


// 解析
// listfield -> exp
static void listfield (LexState *ls, ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}


// 解析
// field -> listfield | recfield
static void field (LexState *ls, ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      // 有可能是数组字段或哈希字段
      if (luaX_lookahead(ls) != '=')  /* expression? */
        // 数组字段
        listfield(ls, cc);
      else
        // 哈希字段
        recfield(ls, cc);
      break;
    }
    case '[': {
      // 哈希字段
      recfield(ls, cc);
      break;
    }
    default: {
	  // 数组字段
      listfield(ls, cc);
      break;
    }
  }
}


// 解析
// constructor -> '{' [ field { sep field } [sep] ] '}'
// sep -> ',' | ';'
static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  // 并未显式指定寄存器索引，后面会luaK_settablesize更新OP_NEWTABLE指令的参数
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  ConsControl cc;
  // 预留一个额外的指令空间，用于后续可能需要的参数，OP_EXTRAARG准备
  luaK_code(fs, 0);  /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  // 给表分配一个寄存器，VNONRELOC
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  // 上面都分配了，这里需要往后挪
  luaK_reserveregs(fs, 1);
  // 当前还没有解析表字段
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  checknext(ls, '{');
  // 使用循环解析表的字段
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    if (ls->t.token == '}') break;
    // 处理数组字段的缓冲区
    closelistfield(fs, &cc);
    // 解析单个字段
    field(ls, &cc);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  // 将数组字段缓冲区中剩余字段加入表中
  lastlistfield(fs, &cc);
  // 设置表大小
  luaK_settablesize(fs, pc, t->u.info, cc.na, cc.nh);
}

/* }====================================================================== */


// 设置函数可变参数，生成指令
static void setvararg (FuncState *fs, int nparams) {
  fs->f->is_vararg = 1;
  luaK_codeABC(fs, OP_VARARGPREP, nparams, 0, 0);
}


// 解析
// parlist -> [ {NAME ','} (NAME | '...') ]
static void parlist (LexState *ls) {
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  // 固定参数个数
  int nparams = 0;
  // 是否有可变参数
  int isvararg = 0;
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    // 说明参数不为空
    do {
      switch (ls->t.token) {
        case TK_NAME: {
          // 创建局部变量
          new_localvar(ls, str_checkname(ls));
          nparams++;
          break;
        }
        case TK_DOTS: {
          luaX_next(ls);
          isvararg = 1;
          break;
        }
        default: luaX_syntaxerror(ls, "<name> or '...' expected");
      }
    } while (!isvararg && testnext(ls, ','));
  }
  // 局部变量写入作用域
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  if (isvararg)
    // 
    setvararg(fs, f->numparams);  /* declared vararg */
  // 预分配寄存器空间
  luaK_reserveregs(fs, fs->nactvar);  /* reserve registers for parameters */
}


// 解析函数体
// ‘(’ [parlist] ‘)’ block end
static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  /* body ->  '(' parlist ')' block END */
  FuncState new_fs;
  BlockCnt bl;
  // 新函数创建proto
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  // 进入其block
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  // 解析参数
  parlist(ls);
  checknext(ls, ')');
  // 解析函数体
  statlist(ls);
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  // 生成创建闭包的字节码指令
  codeclosure(ls, e);
  // 函数编译完成了，关闭函数
  close_func(ls);
}


// 解析表达式列表，返回参数个数
// explist ::= exp {‘,’ exp}
static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  // 参数个数，至少一个
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    // 下面还要用v呢，所以这里需要放到寄存器中
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


// 解析
// funcargs ::= '(' [ explist ] ')' | constructor | STRING
static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  // 函数参数表达式
  expdesc args;
  // 目标函数在寄存器中的位置，参数的数量
  int base, nparams;
  // 当前行号，用于错误报告和调试信息
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        // 参数列表为空
        args.k = VVOID;
      else {
        explist(ls, &args);
        // 如果最后一个表达式是多返回值，
        if (hasmultret(args.k))
          luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': {  /* funcargs -> constructor */
      // 参数是一个表构造器
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      // 参数是字符串字面量
      codestring(&args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  // 调用函数在寄存器的位置
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    // 如果存在参数，这里处理最后一个参数，将最后一个参数固定到寄存器
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    // 计算参数个数，fs->freereg-1就是最后一个参数的位置，如果是1表示没参数
    nparams = fs->freereg - (base+1);
  }
  // c=2暂时假设是一个返回值吧
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  // 修正：将生成的字节码指令与源代码的行号line关联起来
  luaK_fixline(fs, line);
  // 函数调用完成后函数和其参数都会被从栈中移除，假设只有一个返回值，所以这里只保留一个
  fs->freereg = base+1;  /* call removes function and arguments and leaves
                            one result (unless changed later) */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


// 解析
// NAME | '(' expr ')'
static void primaryexp (LexState *ls, expdesc *v) {
  /* primaryexp -> NAME | '(' expr ')' */
  switch (ls->t.token) {
    case '(': {
      int line = ls->linenumber;
      luaX_next(ls);
      expr(ls, v);
      check_match(ls, ')', '(', line);
      luaK_dischargevars(ls->fs, v);
      return;
    }
    case TK_NAME: {
      singlevar(ls, v);
      return;
    }
    default: {
      luaX_syntaxerror(ls, "unexpected symbol");
    }
  }
}


// 解析
// primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs }
static void suffixedexp (LexState *ls, expdesc *v) {
  /* suffixedexp ->
       primaryexp { '.' NAME | '[' exp ']' | ':' NAME funcargs | funcargs } */
  FuncState *fs = ls->fs;
  primaryexp(ls, v);
  for (;;) {
    switch (ls->t.token) {
      case '.': {  /* fieldsel */
        fieldsel(ls, v);
        break;
      }
      case '[': {  /* '[' exp ']' */
        expdesc key;
        luaK_exp2anyregup(fs, v);
        yindex(ls, &key);
        // 生成表索引操作t[k]表达式到v
        luaK_indexed(fs, v, &key);
        break;
      }
      case ':': {  /* ':' NAME funcargs */
        expdesc key;
        luaX_next(ls);
        codename(ls, &key);
        luaK_self(fs, v, &key);
        funcargs(ls, v);
        break;
      }
      case '(': case TK_STRING: case '{': {  /* funcargs */
        luaK_exp2nextreg(fs, v);
        funcargs(ls, v);
        break;
      }
      default: return;
    }
  }
}


// 解析
/* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                constructor | FUNCTION body | suffixedexp */
static void simpleexp (LexState *ls, expdesc *v) {
  /* simpleexp -> FLT | INT | STRING | NIL | TRUE | FALSE | ... |
                  constructor | FUNCTION body | suffixedexp */
  switch (ls->t.token) {
    case TK_FLT: {
      init_exp(v, VKFLT, 0);
      v->u.nval = ls->t.seminfo.r;
      break;
    }
    case TK_INT: {
      init_exp(v, VKINT, 0);
      v->u.ival = ls->t.seminfo.i;
      break;
    }
    case TK_STRING: {
      codestring(v, ls->t.seminfo.ts);
      break;
    }
    case TK_NIL: {
      init_exp(v, VNIL, 0);
      break;
    }
    case TK_TRUE: {
      init_exp(v, VTRUE, 0);
      break;
    }
    case TK_FALSE: {
      init_exp(v, VFALSE, 0);
      break;
    }
    case TK_DOTS: {  /* vararg */
      FuncState *fs = ls->fs;
      check_condition(ls, fs->f->is_vararg, 
                      "cannot use '...' outside a vararg function");
      init_exp(v, VVARARG, luaK_codeABC(fs, OP_VARARG, 0, 0, 1));
      break;
    }
    case '{': {  /* constructor */
      constructor(ls, v);
      return;
    }
    case TK_FUNCTION: {
      luaX_next(ls);
      body(ls, v, 0, ls->linenumber);
      return;
    }
    default: {
      suffixedexp(ls, v);
      return;
    }
  }
  luaX_next(ls);
}


// 根据tokenId获取当前一元操作
static UnOpr getunopr (int op) {
  switch (op) {
    // not
    case TK_NOT: return OPR_NOT;
    // 负数
    case '-': return OPR_MINUS;
    // 非
    case '~': return OPR_BNOT;
    // 求长度
    case '#': return OPR_LEN;
    // 不是一元操作符
    default: return OPR_NOUNOPR;
  }
}


// 根据tokenId获取当前二元操作
static BinOpr getbinopr (int op) {
  switch (op) {
    // 加
    case '+': return OPR_ADD;
    // 减
    case '-': return OPR_SUB;
    // 乘
    case '*': return OPR_MUL;
    // 模
    case '%': return OPR_MOD;
    // 幂
    case '^': return OPR_POW;
    // 除
    case '/': return OPR_DIV;
    // 整除
    case TK_IDIV: return OPR_IDIV;
    // 与
    case '&': return OPR_BAND;
    // 或
    case '|': return OPR_BOR;
    // 异或
    case '~': return OPR_BXOR;
    // 左移
    case TK_SHL: return OPR_SHL;
    // 右移
    case TK_SHR: return OPR_SHR;
    // 连接
    case TK_CONCAT: return OPR_CONCAT;
    // 不等于
    case TK_NE: return OPR_NE;
    // 等于
    case TK_EQ: return OPR_EQ;
    // 小于
    case '<': return OPR_LT;
    // 小于等于
    case TK_LE: return OPR_LE;
    // 大于
    case '>': return OPR_GT;
    // 大于等于
    case TK_GE: return OPR_GE;
    // and
    case TK_AND: return OPR_AND;
    // or
    case TK_OR: return OPR_OR;
    // 不是二元操作
    default: return OPR_NOBINOPR;
  }
}


/*
** Priority table for binary operators.
*/
/*
* Lua 运算符的优先级从低到高如下表所示：
*     or
*     and
*     <     >     <=    >=    ~=    ==
*     |
*     ~
*     &
*     <<    >>
*     ..
*     +     -
*     *     /     //    %
*     一元运算符（not   #     -     ~）
*     ^
* 通常，可以使用括号改变表达式的优先级。连接运算符（'..'）和幂运算符（'^'）是右结合的，其他所有二元运算符均为左结合。
*/
// 二元运算符的优先级表
static const struct {
  // 若 left > right：右结合
  // 若 left == right：左结合（其他运算符）
  lu_byte left;  /* left priority for each binary operator */
  lu_byte right; /* right priority */
} priority[] = {  /* ORDER OPR */
   {10, 10}, {10, 10},           /* '+' '-' */
   {11, 11}, {11, 11},           /* '*' '%' */
   {14, 13},                  /* '^' (right associative) */
   {11, 11}, {11, 11},           /* '/' '//' */
   {6, 6}, {4, 4}, {5, 5},   /* '&' '|' '~' */
   {7, 7}, {7, 7},           /* '<<' '>>' */
   {9, 8},                   /* '..' (right associative) */
   {3, 3}, {3, 3}, {3, 3},   /* ==, <, <= */
   {3, 3}, {3, 3}, {3, 3},   /* ~=, >, >= */
   {2, 2}, {1, 1}            /* and, or */
};

#define UNARY_PRIORITY	12  /* priority for unary operators */


/*
** subexpr -> (simpleexp | unop subexpr) { binop subexpr }
** where 'binop' is any binary operator with a priority higher than 'limit'
*/
// 解析表达式
// 返回值为第一个未处理的二元运算符
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop;
  // 对当前调用深度进行检测，如果递归太深，编译会报错
  // 表达式可能深度嵌套（例如 1 + 2 * (3 - 4 / (5 + 6))），导致解析器递归调用过深，引发栈溢出。
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  // 看看是否有一元操作
  if (uop != OPR_NOUNOPR) {  /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    subexpr(ls, v, UNARY_PRIORITY);
    // 处理一元操作符了
    luaK_prefix(ls->fs, uop, v, line);
  }
  else simpleexp(ls, v);
  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  // 是二元运算符，且优先级大于limit
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    // 右侧子表达式的描述信息
    expdesc v2;
    // 右侧表达式对应的二元运算符
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    // 据二元运算符的类型，处理左操作数v
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    // 解析右侧的子表达式
    nextop = subexpr(ls, &v2, priority[op].right);
    // 计算二元运算符表达式写入v
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}


// 解析表达式
static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


// 进入block
static void block (LexState *ls) {
  /* block -> statlist */
  FuncState *fs = ls->fs;
  BlockCnt bl;
  enterblock(fs, &bl, 0);
  statlist(ls);
  leaveblock(fs);
}


/*
** structure to chain all variables in the left-hand side of an
** assignment
*/
// 管理赋值语句中左侧的所有变量（如全局变量、局部变量、上值或索引变量）
struct LHS_assign {
  // prev 是一个指向另一个LHS_assign结构体的指针，用于构建链表。
  // 它指向当前变量节点的前一个变量节点，从而形成一个单向链表
  struct LHS_assign *prev;
  // 全局，本地，上值，索引
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
// 检测在多重赋值语句中是否存在潜在的变量冲突
// lua多重赋值允许同时对多个变量进行赋值。如果某个变量既是左侧的目标变量，又在右侧表达式中被使用，可能会导致意外的行为
// 为了确保赋值逻辑正确，编译器会将原始的t存储到一个临时寄存器中，作为“安全副本”。具体步骤如下：
// 分配临时寄存器 ：
// 编译器预留一个寄存器用于存储安全副本。
// 复制原始值 ：
// 如果t是局部变量，则生成OP_MOVE指令，将t的值复制到临时寄存器。
// 如果t是上值，则生成OP_GETUPVAL指令，将上值加载到临时寄存器。
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  // 用于存储安全副本的寄存器索引
  int extra = fs->freereg;  /* eventual position to save local variable */
  // 是否冲突标记
  int conflict = 0;
  // 遍历检查所有之前的赋值操作
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    // 是否是表索引
    if (vkisindexed(lh->v.k)) {  /* assignment to table field? */
      // 是表索引并且是这个表在上值中，key在常量表中字符串索引
      // local t = {}; local function inner() t["1"], t = 20, {}; end
      if (lh->v.k == VINDEXUP) {  /* is table an upvalue? */
        // 检查当前变量v是否为上值，并且是否与表字段的表相同
        if (v->k == VUPVAL && lh->v.u.ind.t == v->u.info) {
          // 确实冲突
          conflict = 1;  /* table is the upvalue being assigned now */
          // t.k，t是表，k在常量表中字符串，
          // ind.t = t 寄存器下标
          // ind.idx = k在常量表中的下标
          lh->v.k = VINDEXSTR;
          // 将表字段的表更新为安全副本的寄存器索引
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
      }
      else {  /* table is a register */
        // 其他，表肯定是在寄存器中了
        // 当前变量是本地变量 && 表对应的寄存器和当前变量的寄存器索引相同
        // local t = {}; local x = 10; t[x], t = x, {}
        if (v->k == VLOCAL && lh->v.u.ind.t == v->u.var.ridx) {
          conflict = 1;  /* table is the local being assigned now */
          // 表对应的寄存器修改为安全副本的寄存器索引
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        // 是表索，存储在寄存器中 && 当前变量是本地变量 && 表索引中key和当前变量的寄存器索引相同
        // local t = {}; local x = 10; t[x], x = x, 20
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.u.ind.idx == v->u.var.ridx) {
          conflict = 1;
          // 表索引中key对应的寄存器修改为安全副本的寄存器索引
          lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      // 局部变量
      // A B	R[A] := R[B]
      luaK_codeABC(fs, OP_MOVE, extra, v->u.var.ridx, 0);
    else
      // 上值
      // A B	R[A] := UpValue[B]
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->u.info, 0);
    // 预留寄存器空间
    luaK_reserveregs(fs, 1);
  }
}

/*
** Parse and compile a multiple assignment. The first "variable"
** (a 'suffixedexp') was already read by the caller.
**
** assignment -> suffixedexp restassign
** restassign -> ',' suffixedexp restassign | '=' explist
*/
// 解析和编译多重赋值语句（multiple assignment）。
// 主要作用是递归地处理赋值语句左侧的变量链表，并确保右侧表达式的数量与左侧变量的数量匹配
// lh当前赋值语句左侧变量的链表节点
// nvars当前左侧变量的数量
static void restassign (LexState *ls, struct LHS_assign *lh, int nvars) {
  // 用于存储右侧表达式的信息，如果是多重赋值语句，必然是最后一个表达式
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  // 确保变量不是只读的
  check_readonly(ls, &lh->v);
  // 如果当前符号是逗号（,），表示还有更多的变量需要处理。
  // 调用suffixedexp解析下一个变量，并将其添加到链表中
  if (testnext(ls, ',')) {  /* restassign -> ',' suffixedexp restassign */
    // 创建一个新的LHS_assign节点nv，并将其链接到当前节点lh
    struct LHS_assign nv;
    nv.prev = lh;
    // 调用suffixedexp解析下一个变量，并将其存储在nv.v中
    suffixedexp(ls, &nv.v);
    // 如果新变量不是索引变量
    if (!vkisindexed(nv.v.k))
      // 检测上一个变量和当前变量是否存在冲突
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls);  /* control recursion depth */
    // 递归调用restassign处理剩余的变量，如果没有剩余变量就处理自身的赋值
    restassign(ls, &nv, nvars+1);
    leavelevel(ls);
  }
  else {  /* restassign -> '=' explist */
    int nexps;
    checknext(ls, '=');
    // 调用explist解析右侧的表达式列表，并返回表达式的数量，e是右侧最后一个表达式
    nexps = explist(ls, &e);
    // 左右两侧不一样，调平一下
    if (nexps != nvars)
      // 调整左右两侧的数量，会生成或者调整已有的指令
      adjust_assign(ls, nvars, nexps, &e);
    else {
      // 左右两侧一样，肯定需要确保最后一个表达式是一个单一值
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      // 将表达式的值存储到左侧变量中
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  // 1.右侧表达式列表为空或者右侧表达式数量少于左侧变量数量
  // 肯定调用adjust_assign进行调整，也预留好了所需寄存器，这里只是给左侧变量赋值，同样是回溯右侧从右向左给左侧从右向左赋值
  // 2.左右两侧相等
  // 这里肯定是回溯右侧从右向左给左侧从右向左赋值，ls->fs->freereg-1肯定打算要给左侧赋值的寄存器
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  // 将表达式的值存储到左侧变量中
  luaK_storevar(ls->fs, &lh->v, &e);
}


static int cond (LexState *ls) {
  /* cond -> exp */
  expdesc v;
  expr(ls, &v);  /* read condition */
  if (v.k == VNIL) v.k = VFALSE;  /* 'falses' are all equal here */
  luaK_goiftrue(ls->fs, &v);
  return v.f;
}


// 解析goto
// goto Name
static void gotostat (LexState *ls) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  TString *name = str_checkname(ls);  /* label's name */
  // 查看标签是否已经定义
  Labeldesc *lb = findlabel(ls, name);
  if (lb == NULL)  /* no label? */
    /* forward jump; will be resolved when the label is declared */
    // 前向跳转
    newgotoentry(ls, name, line, luaK_jump(fs));
  else {  /* found a label */
    /* backward jump; will be resolved here */
    int lblevel = reglevel(fs, lb->nactvar);  /* label level */
    if (luaY_nvarstack(fs) > lblevel)  /* leaving the scope of a variable? */
      luaK_codeABC(fs, OP_CLOSE, lblevel, 0, 0);
    /* create jump and link it to the label */
    luaK_patchlist(fs, luaK_jump(fs), lb->pc);
  }
}


/*
** Break statement. Semantically equivalent to "goto break".
*/
static void breakstat (LexState *ls) {
  int line = ls->linenumber;
  luaX_next(ls);  /* skip break */
  newgotoentry(ls, luaS_newliteral(ls->L, "break"), line, luaK_jump(ls->fs));
}


/*
** Check whether there is already a label with the given 'name'.
*/
// 检查当前作用域中是否已经存在同名标签
static void checkrepeated (LexState *ls, TString *name) {
  Labeldesc *lb = findlabel(ls, name);
  if (l_unlikely(lb != NULL)) {  /* already defined? */
    const char *msg = "label '%s' already defined on line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(name), lb->line);
    luaK_semerror(ls, msg);  /* error */
  }
}


// 解析
// label -> '::' NAME '::'
static void labelstat (LexState *ls, TString *name, int line) {
  /* label -> '::' NAME '::' */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    // ::middle:: ; ::empty:: ;; 其他语句
    statement(ls);  /* skip other no-op statements */
  // 检查当前作用域中是否已经存在同名标签
  checkrepeated(ls, name);  /* check for repeated labels */
  // 创建标签，并将其注册到当前作用域中
  createlabel(ls, name, line, block_follow(ls, 0));
}


static void whilestat (LexState *ls, int line) {
  /* whilestat -> WHILE cond DO block END */
  FuncState *fs = ls->fs;
  int whileinit;
  int condexit;
  BlockCnt bl;
  luaX_next(ls);  /* skip WHILE */
  whileinit = luaK_getlabel(fs);
  condexit = cond(ls);
  enterblock(fs, &bl, 1);
  checknext(ls, TK_DO);
  block(ls);
  luaK_jumpto(fs, whileinit);
  check_match(ls, TK_END, TK_WHILE, line);
  leaveblock(fs);
  luaK_patchtohere(fs, condexit);  /* false conditions finish the loop */
}


static void repeatstat (LexState *ls, int line) {
  /* repeatstat -> REPEAT block UNTIL cond */
  int condexit;
  FuncState *fs = ls->fs;
  int repeat_init = luaK_getlabel(fs);
  BlockCnt bl1, bl2;
  enterblock(fs, &bl1, 1);  /* loop block */
  enterblock(fs, &bl2, 0);  /* scope block */
  luaX_next(ls);  /* skip REPEAT */
  statlist(ls);
  check_match(ls, TK_UNTIL, TK_REPEAT, line);
  condexit = cond(ls);  /* read condition (inside scope block) */
  leaveblock(fs);  /* finish scope */
  if (bl2.upval) {  /* upvalues? */
    int exit = luaK_jump(fs);  /* normal exit must jump over fix */
    luaK_patchtohere(fs, condexit);  /* repetition must close upvalues */
    luaK_codeABC(fs, OP_CLOSE, reglevel(fs, bl2.nactvar), 0, 0);
    condexit = luaK_jump(fs);  /* repeat after closing upvalues */
    luaK_patchtohere(fs, exit);  /* normal exit comes to here */
  }
  luaK_patchlist(fs, condexit, repeat_init);  /* close the loop */
  leaveblock(fs);  /* finish loop */
}


/*
** Read an expression and generate code to put its results in next
** stack slot.
**
*/
static void exp1 (LexState *ls) {
  expdesc e;
  expr(ls, &e);
  luaK_exp2nextreg(ls->fs, &e);
  lua_assert(e.k == VNONRELOC);
}


/*
** Fix for instruction at position 'pc' to jump to 'dest'.
** (Jump addresses are relative in Lua). 'back' true means
** a back jump.
*/
static void fixforjump (FuncState *fs, int pc, int dest, int back) {
  Instruction *jmp = &fs->f->code[pc];
  int offset = dest - (pc + 1);
  if (back)
    offset = -offset;
  if (l_unlikely(offset > MAXARG_Bx))
    luaX_syntaxerror(fs->ls, "control structure too long");
  SETARG_Bx(*jmp, offset);
}


/*
** Generate code for a 'for' loop.
*/
static void forbody (LexState *ls, int base, int line, int nvars, int isgen) {
  /* forbody -> DO block */
  static const OpCode forprep[2] = {OP_FORPREP, OP_TFORPREP};
  static const OpCode forloop[2] = {OP_FORLOOP, OP_TFORLOOP};
  BlockCnt bl;
  FuncState *fs = ls->fs;
  int prep, endfor;
  checknext(ls, TK_DO);
  prep = luaK_codeABx(fs, forprep[isgen], base, 0);
  enterblock(fs, &bl, 0);  /* scope for declared variables */
  adjustlocalvars(ls, nvars);
  luaK_reserveregs(fs, nvars);
  block(ls);
  leaveblock(fs);  /* end of scope for declared variables */
  fixforjump(fs, prep, luaK_getlabel(fs), 0);
  if (isgen) {  /* generic for? */
    luaK_codeABC(fs, OP_TFORCALL, base, 0, nvars);
    luaK_fixline(fs, line);
  }
  endfor = luaK_codeABx(fs, forloop[isgen], base, 0);
  fixforjump(fs, endfor, prep + 1, 1);
  luaK_fixline(fs, line);
}


static void fornum (LexState *ls, TString *varname, int line) {
  /* fornum -> NAME = exp,exp[,exp] forbody */
  FuncState *fs = ls->fs;
  int base = fs->freereg;
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvar(ls, varname);
  checknext(ls, '=');
  exp1(ls);  /* initial value */
  checknext(ls, ',');
  exp1(ls);  /* limit */
  if (testnext(ls, ','))
    exp1(ls);  /* optional step */
  else {  /* default step = 1 */
    luaK_int(fs, fs->freereg, 1);
    luaK_reserveregs(fs, 1);
  }
  adjustlocalvars(ls, 3);  /* control variables */
  forbody(ls, base, line, 1, 0);
}


static void forlist (LexState *ls, TString *indexname) {
  /* forlist -> NAME {,NAME} IN explist forbody */
  FuncState *fs = ls->fs;
  expdesc e;
  int nvars = 5;  /* gen, state, control, toclose, 'indexname' */
  int line;
  int base = fs->freereg;
  /* create control variables */
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  new_localvarliteral(ls, "(for state)");
  /* create declared variables */
  new_localvar(ls, indexname);
  while (testnext(ls, ',')) {
    new_localvar(ls, str_checkname(ls));
    nvars++;
  }
  checknext(ls, TK_IN);
  line = ls->linenumber;
  adjust_assign(ls, 4, explist(ls, &e), &e);
  adjustlocalvars(ls, 4);  /* control variables */
  marktobeclosed(fs);  /* last control var. must be closed */
  luaK_checkstack(fs, 3);  /* extra space to call generator */
  forbody(ls, base, line, nvars - 4, 1);
}


static void forstat (LexState *ls, int line) {
  /* forstat -> FOR (fornum | forlist) END */
  FuncState *fs = ls->fs;
  TString *varname;
  BlockCnt bl;
  enterblock(fs, &bl, 1);  /* scope for loop and control variables */
  luaX_next(ls);  /* skip 'for' */
  varname = str_checkname(ls);  /* first variable name */
  switch (ls->t.token) {
    case '=': fornum(ls, varname, line); break;
    case ',': case TK_IN: forlist(ls, varname); break;
    default: luaX_syntaxerror(ls, "'=' or 'in' expected");
  }
  check_match(ls, TK_END, TK_FOR, line);
  leaveblock(fs);  /* loop scope ('break' jumps to this point) */
}


// 解析
// test_then_block -> [IF | ELSEIF] cond THEN block
// escapelist跳转列表，用于存储需要跳过的分支的跳转指令
static void test_then_block (LexState *ls, int *escapelist) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  // 条件表达式的描述信息
  expdesc v;
  // 假跳转列表
  int jf;  /* instruction to skip 'then' code (if condition is false) */
  luaX_next(ls);  /* skip IF or ELSEIF */
  // 解析条件表达式
  expr(ls, &v);  /* read condition */
  checknext(ls, TK_THEN);
  if (ls->t.token == TK_BREAK) {  /* 'if x then break' ? */
    // 处理if x then break
    int line = ls->linenumber;
    luaK_goiffalse(ls->fs, &v);  /* will jump if condition is true */
    luaX_next(ls);  /* skip 'break' */
    enterblock(fs, &bl, 0);  /* must enter block before 'goto' */
    newgotoentry(ls, luaS_newliteral(ls->L, "break"), line, v.t);
    while (testnext(ls, ';')) {}  /* skip semicolons */
    if (block_follow(ls, 0)) {  /* jump is the entire block? */
      leaveblock(fs);
      return;  /* and that is it */
    }
    else  /* must skip over 'then' part if condition is false */
      jf = luaK_jump(fs);
  }
  else {  /* regular case (not a break) */
    // 确保在条件为假时跳过then部分的代码，为真的时候继续，当条件为假时跳过then块
    luaK_goiftrue(ls->fs, &v);  /* skip over block if condition is false */
    // 进入then block
    enterblock(fs, &bl, 0);
    // 记录假跳转列表
    jf = v.f;
  }
  // 解析then部分
  statlist(ls);  /* 'then' part */
  // 离开then block
  leaveblock(fs);
  // 看看后面是否有else/elseif语句
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
    // 跳过后续的else/elseif部分
    // 生成一条跳转指令，并将其添加到跳转列表（escapelist）中 ，以确保在THEN块执行完成后能够跳过后续的ELSE或ELSEIF部分
    // 但是目前而言肯定不知道这个跳转指令到底跳过了什么，需要后续修补
    luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
  // 修补上面的假跳转，跳到这个位置吧
  luaK_patchtohere(fs, jf);
}


// 解析
// ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END
static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  // 跳过else/elseif的跳转指令
  int escapelist = NO_JUMP;  /* exit list for finished parts */
  test_then_block(ls, &escapelist);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist);  /* ELSEIF cond THEN block */
  if (testnext(ls, TK_ELSE))
    block(ls);  /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  // 将escapelist中的所有跳转指令修补到if语句的末尾
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


// 解析
// local function Name funcbody
static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* function's variable index */
  // ls->dyd创建新的局部变量
  new_localvar(ls, str_checkname(ls));  /* new local variable */
  // 放入作用域，给上面变量赋值，var->vd.ridx和var->vd.pidx
  adjustlocalvars(ls, 1);  /* enter its scope */
  // 解析funcbody
  body(ls, &b, 0, ls->linenumber);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}


// 获取局部变量<>里面的属性
static int getlocalattribute (LexState *ls) {
  /* ATTRIB -> ['<' Name '>'] */
  if (testnext(ls, '<')) {
    const char *attr = getstr(str_checkname(ls));
    checknext(ls, '>');
    if (strcmp(attr, "const") == 0)
      return RDKCONST;  /* read-only variable */
    else if (strcmp(attr, "close") == 0)
      return RDKTOCLOSE;  /* to-be-closed variable */
    else
      luaK_semerror(ls,
        luaO_pushfstring(ls->L, "unknown attribute '%s'", attr));
  }
  return VDKREG;  /* regular variable */
}


// 检查to-be-closed变量，生成OP_TBC指令
static void checktoclose (FuncState *fs, int level) {
  if (level != -1) {  /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}


// 解析
// stat -> LOCAL NAME ATTRIB { ',' NAME ATTRIB } ['=' explist]
static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME ATTRIB { ',' NAME ATTRIB } ['=' explist] */
  FuncState *fs = ls->fs;
  // 
  int toclose = -1;  /* index of to-be-closed variable (if any) */
  // 最后一个变量描述
  Vardesc *var;  /* last variable */
  // 当前定义变量下标和属性类型
  int vidx, kind;  /* index and kind of last variable */
  // 定义的变量个数
  int nvars = 0;
  // 赋值表达式的数量
  int nexps;
  // explist的表达式，第一个或者最后一个
  expdesc e;
  do {
    // 放到ls->dyd->actvar.arr中，返回下标
    vidx = new_localvar(ls, str_checkname(ls));
    // 看看有没有额外属性
    kind = getlocalattribute(ls);
    // 设置类型
    getlocalvardesc(fs, vidx)->vd.kind = kind;
    if (kind == RDKTOCLOSE) {  /* to-be-closed? */
      if (toclose != -1)  /* one already present? */
        luaK_semerror(ls, "multiple to-be-closed variables in local list");
      toclose = fs->nactvar + nvars;
    }
    nvars++;
  } while (testnext(ls, ','));
  if (testnext(ls, '='))
    nexps = explist(ls, &e);
  else {
    // 没有右侧赋值表达式
    e.k = VVOID;
    nexps = 0;
  }
  // 最后一个变量
  var = getlocalvardesc(fs, vidx);  /* get last variable */
  // 定义变量和赋值表达式数量一致 &&
  // 最后一个变量是常量 &&
  // 表达式是一个常量，进行优化如var—>k中
  if (nvars == nexps &&  /* no adjustments? */
      var->vd.kind == RDKCONST &&  /* last variable is const? */
      luaK_exp2const(fs, &e, &var->k)) {  /* compile-time constant? */
    var->vd.kind = RDKCTC;  /* variable is a compile-time constant */
    // 这个常量肯定不需要分配寄存器索引，所以这里排除自己，只注册前面的变量
    adjustlocalvars(ls, nvars - 1);  /* exclude last variable */
    // 但是当前作用域的变量个数需要+1
    fs->nactvar++;  /* but count it */
  }
  else {
    // 调整nvars和nexps，会生成或者调整已有的指令
    adjust_assign(ls, nvars, nexps, &e);
    // 将新声明的局部变量注册到当前作用域，并为其分配寄存器索引
    adjustlocalvars(ls, nvars);
  }
  // 检查to-be-closed变量，生成OP_TBC指令
  checktoclose(fs, toclose);
}


static int funcname (LexState *ls, expdesc *v) {
  /* funcname -> NAME {fieldsel} [':' NAME] */
  int ismethod = 0;
  singlevar(ls, v);
  while (ls->t.token == '.')
    fieldsel(ls, v);
  if (ls->t.token == ':') {
    ismethod = 1;
    fieldsel(ls, v);
  }
  return ismethod;
}


static void funcstat (LexState *ls, int line) {
  /* funcstat -> FUNCTION funcname body */
  int ismethod;
  expdesc v, b;
  luaX_next(ls);  /* skip FUNCTION */
  ismethod = funcname(ls, &v);
  body(ls, &b, ismethod, line);
  check_readonly(ls, &v);
  luaK_storevar(ls->fs, &v, &b);
  luaK_fixline(ls->fs, line);  /* definition "happens" in the first line */
}


// 解析
// func | assignment
static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  // 存储左侧赋值表达式的信息
  struct LHS_assign v;
  // 先解析左侧的第一个表达式
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    // 赋值语句
    v.prev = NULL;
    // 处理多重赋值，1表示目前是一个左侧变量数量
    restassign(ls, &v, 1);
  }
  else {  /* stat -> func */
    // 函数调用语句
    Instruction *inst;
    check_condition(ls, v.v.k == VCALL, "syntax error");
    inst = &getinstruction(fs, &v.v);
    // 重新把c设置成1，没有返回值
    SETARG_C(*inst, 1);  /* call statement uses no results */
  }
}


// 解析
// stat -> RETURN [explist] [';']
static void retstat (LexState *ls) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  // 存储返回值的表达式信息
  expdesc e;
  // 返回值个数
  int nret;  /* number of values being returned */
  // 这里返回的是本地变量个数，也就是第一个返回值位置
  int first = luaY_nvarstack(fs);  /* first slot to be returned */
  if (block_follow(ls, 1) || ls->t.token == ';')
    nret = 0;  /* return no values */
  else {
    nret = explist(ls, &e);  /* optional return values */
    if (hasmultret(e.k)) {
      luaK_setmultret(fs, &e);
      if (e.k == VCALL && nret == 1 && !fs->bl->insidetbc) {  /* tail call? */
        SET_OPCODE(getinstruction(fs,&e), OP_TAILCALL);
        lua_assert(GETARG_A(getinstruction(fs,&e)) == luaY_nvarstack(fs));
      }
      nret = LUA_MULTRET;  /* return all values */
    }
    else {
      if (nret == 1)  /* only one single value? */
        first = luaK_exp2anyreg(fs, &e);  /* can use original slot */
      else {  /* values must go to the top of the stack */
        luaK_exp2nextreg(fs, &e);
        lua_assert(nret == fs->freereg - first);
      }
    }
  }
  luaK_ret(fs, first, nret);
  testnext(ls, ';');  /* skip optional semicolon */
}


// 解析state
static void statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  // 对当前调用深度进行检测，如果递归太深，编译会报错
  enterlevel(ls);
  switch (ls->t.token) {
    case ';': {  /* stat -> ';' (empty statement) */
      luaX_next(ls);  /* skip ';' */
      break;
    }
    case TK_IF: {  /* stat -> ifstat */
      ifstat(ls, line);
      break;
    }
    case TK_WHILE: {  /* stat -> whilestat */
      whilestat(ls, line);
      break;
    }
    case TK_DO: {  /* stat -> DO block END */
      luaX_next(ls);  /* skip DO */
      block(ls);
      check_match(ls, TK_END, TK_DO, line);
      break;
    }
    case TK_FOR: {  /* stat -> forstat */
      forstat(ls, line);
      break;
    }
    case TK_REPEAT: {  /* stat -> repeatstat */
      repeatstat(ls, line);
      break;
    }
    case TK_FUNCTION: {  /* stat -> funcstat */
      funcstat(ls, line);
      break;
    }
    case TK_LOCAL: { /* stat -> localstat */
      luaX_next(ls); /* skip LOCAL */
      if (testnext(ls, TK_FUNCTION))  /* local function? */
        localfunc(ls);
      else
        localstat(ls);
      break;
    }
    case TK_DBCOLON: {  /* stat -> label */
      luaX_next(ls);  /* skip double colon */
      labelstat(ls, str_checkname(ls), line);
      break;
    }
    case TK_RETURN: {  /* stat -> retstat */
      luaX_next(ls);  /* skip RETURN */
      retstat(ls);
      break;
    }
    case TK_BREAK: {  /* stat -> breakstat */
      breakstat(ls);
      break;
    }
    case TK_GOTO: {  /* stat -> 'goto' NAME */
      luaX_next(ls);  /* skip 'goto' */
      gotostat(ls);
      break;
    }
    default: {  /* stat -> func | assignment */
      exprstat(ls);
      break;
    }
  }
  lua_assert(ls->fs->f->maxstacksize >= ls->fs->freereg &&
             ls->fs->freereg >= luaY_nvarstack(ls->fs));
  // 将当前函数的寄存器使用情况重置为局部变量的数量
  ls->fs->freereg = luaY_nvarstack(ls->fs);  /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
// 编译主函数
static void mainfunc (LexState *ls, FuncState *fs) {
  // top-level function block
  BlockCnt bl;
  Upvaldesc *env;
  // 进入top-level block
  open_func(ls, fs, &bl);
  // 主函数是变参，需要生成OP_VARARGPREP
  setvararg(fs, 0);  /* main function is always declared vararg */
  // 一个Lua文件在载入的时候会先创建出一个最顶层（Top level）的Lua闭包，
  // 该闭包默认带有一个UpValue，这个UpValue的变量名为"_ENV"，
  // 它指向Lua虚拟机的全局变量表，即_G表，可以理解为_G表即为当前Lua文件中代码的运行环境(env)。
  // 事实上，每一个Lua闭包它们第一个UpValue值都是_ENV。
  env = allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  // 读取第一个token
  luaX_next(ls);  /* read first token */
  // 进入语法分析，解析block
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  // 退出top-level block
  close_func(ls);
}


// 词法语法解析
LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  // 创建main闭包
  LClosure *cl = luaF_newLclosure(L, 1);  /* create main closure */
  setclLvalue2s(L, L->top.p, cl);  /* anchor it (to avoid being collected) */
  luaD_inctop(L);
  // 创建常量缓存查找表
  lexstate.h = luaH_new(L);  /* create table for scanner */
  sethvalue2s(L, L->top.p, lexstate.h);  /* anchor it */
  luaD_inctop(L);
  // 创建main函数原型
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  // 编译主函数
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  L->top.p--;  /* remove scanner's table */
  return cl;  /* closure is on the stack, too */
}

