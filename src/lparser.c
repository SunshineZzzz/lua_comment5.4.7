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
// �ֲ��������������200����
#define MAXVARS		200


// �������û��߿ɱ��������෵��ֵ
#define hasmultret(k)		((k) == VCALL || (k) == VVARARG)


/* because all strings are unified by the scanner, the parser
   can use pointer equality for string equality */
#define eqstr(a,b)	((a) == (b))


/*
** nodes for block list (list of active blocks)
*/
// ����lua�﷨�����е�block������Ϣ
typedef struct BlockCnt {
  // �����block
  struct BlockCnt *previous;  /* chain */
  // ��ǰ�����У���һ����ǩ�����ȫ���������ı�ǩ�е�λ��
  int firstlabel;  /* index of first label in this block */
  // ��ǰ�����У���һ��δƥ���ǩ�����ȫ��δƥ��ı�ǩ�е�λ��
  int firstgoto;  /* index of first pending goto in this block */
  // �ڽ��뱾block��ʱ�������Ծ�ľֲ�����������
  lu_byte nactvar;  /* # active locals outside the block */
  // ��Ŀ����upval���Ϊ1����ʾ�ÿ����ĳЩ�������������upvalues
  // �ÿ�ر�ʱ����Ҫ����
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


// ������ƣ��������ƾͱ���
static void checklimit (FuncState *fs, int v, int l, const char *what) {
  if (v > l) errorlimit(fs, l, what);
}


/*
** Test whether next token is 'c'; if so, skip it.
*/
// ��һ��token�Ƿ���c
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
// ��ǰtoken����c�����׳��쳣
static void check (LexState *ls, int c) {
  if (ls->t.token != c)
    error_expected(ls, c);
}


/*
** Check that next token is 'c' and skip it.
*/
// ��⵱ǰtoken�Ƿ���c��������һ��
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
// what��ʾ�����Ĵʷ���Ԫ����
// who��ʾ��what��ص���һ���ʷ���Ԫ���ͣ�ͨ������������Ҫƥ��������ġ�
// where��ʾwho���ڵ��к�
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


// ��ǰtoken�Ƿ���TK_NAME�����Ǿͻ��׳��쳣������TK_NAME������һ��token
static TString *str_checkname (LexState *ls) {
  TString *ts;
  check(ls, TK_NAME);
  ts = ls->t.seminfo.ts;
  luaX_next(ls);
  return ts;
}


// ��ʼ�����ʽ
static void init_exp (expdesc *e, expkind k, int i) {
  e->f = e->t = NO_JUMP;
  e->k = k;
  e->u.info = i;
}


// ���ڲ��ַ�����ʼ�����ʽe
static void codestring (expdesc *e, TString *s) {
  e->f = e->t = NO_JUMP;
  e->k = VKSTR;
  e->u.strval = s;
}


// ����ʶ������������ת��Ϊ�ַ������������洢�����ʽ��������
static void codename (LexState *ls, expdesc *e) {
  codestring(e, str_checkname(ls));
}


/*
** Register a new local variable in the active 'Proto' (for debug
** information).
*/
// ���������ľֲ�����ע�ᵽ��ǰ�����ĵ�����Ϣ��
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
// �ø�������ע��һ�����ر����������±�
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
// ��ȡlocal��������
static Vardesc *getlocalvardesc (FuncState *fs, int vidx) {
  return &fs->ls->dyd->actvar.arr[fs->firstlocal + vidx];
}


/*
** Convert 'nvar', a compiler index level, to its corresponding
** register. For that, search for the highest variable below that level
** that is in a register and uses its register index ('ridx') plus one.
*/
// ���ݸ�����local�������������ض�Ӧ�ļĴ����㼶(������һ������ʹ�õļĴ����㼶)
static int reglevel (FuncState *fs, int nvar) {
  // ��һ���±�û����Ŷ
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
// ���ص�ǰ���������л������ռ�õļĴ�����Χ�����Ĵ����㼶��
// ��ʵҲ���ǣ����ظ��������ļĴ�����ջ�еı�����
int luaY_nvarstack (FuncState *fs) {
  return reglevel(fs, fs->nactvar);
}


/*
** Get the debug-information entry for current variable 'vidx'.
*/
// ����vidx��ȡ��ǰ�����ĵ�����Ϣ
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
// ��ʼ��һ��local������Ϣ
static void init_var (FuncState *fs, expdesc *e, int vidx) {
  e->f = e->t = NO_JUMP;
  e->k = VLOCAL;
  e->u.var.vidx = vidx;
  e->u.var.ridx = getlocalvardesc(fs, vidx)->vd.ridx;
}


/*
** Raises an error if variable described by 'e' is read only
*/
// �����ֻ���������׳�����
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
// ��ҪĿ���Ƿ���Ĵ�������
// ���������ľֲ�����ע�ᵽ��ǰ�����򣬲�Ϊ�����Ĵ�������
static void adjustlocalvars (LexState *ls, int nvars) {
  FuncState *fs = ls->fs;
  int reglevel = luaY_nvarstack(fs);
  int i;
  for (i = 0; i < nvars; i++) {
    // Ϊ�±�������һ��Ψһ������
    int vidx = fs->nactvar++;
    // ��ȡָ�������ľֲ�����������
    Vardesc *var = getlocalvardesc(fs, vidx);
    // ÿ�η���󣬵���reglevel��ȷ����һ������ʹ����һ���Ĵ���
    var->vd.ridx = reglevel++;
    // ������������ע�ᵽ���ű��У����������ڷ��ű��е�����
    var->vd.pidx = registerlocalvar(ls, fs, var->vd.name);
  }
}


/*
** Close the scope for all variables up to level 'tolevel'.
** (debug info.)
*/
// �رմӵ�ǰ������ָ���㼶tolevel����������б��������Ҹ��µ�����Ϣ
static void removevars (FuncState *fs, int tolevel) {
  // �ܹ���Ծ�������� -= (��ǰ�����Ļ�Ծ�������� - ��ǰ������(block)Ҫ�ر��ˣ������ǽ�Ҫ�ָ���������Ļ�Ծ��������)
  // Ŀ��㼶��Ծ��������
  fs->ls->dyd->actvar.n -= (fs->nactvar - tolevel);
  // ѭ�����õ�����Ϣendpc
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
// �鿴��ǰ�����㼶��ֵ�����и�NAME���еĻ��򷵻ض�Ӧλ�ã��Ҳ�������-1
static int searchupvalue (FuncState *fs, TString *name) {
  int i;
  Upvaldesc *up = fs->f->upvalues;
  for (i = 0; i < fs->nups; i++) {
    if (eqstr(up[i].name, name)) return i;
  }
  return -1;  /* not found */
}


// ����upvalues�����ҷ��ص�һ��Upvaldesc����
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


// ���ֲ���������㺯������ֵ����Ϊ��ǰ��������ֵ���������¼����������ֵ�������б���
static int newupvalue (FuncState *fs, TString *name, expdesc *v) {
  // ����һ���µ���ֵ������
  Upvaldesc *up = allocupvalue(fs);
  FuncState *prev = fs->prev;
  if (v->k == VLOCAL) {
    // �Ǿֲ�����
    // ����ֵ��Դ��ջ�еľֲ�����
    up->instack = 1;
    // ��¼�Ĵ����±�
    up->idx = v->u.var.ridx;
    up->kind = getlocalvardesc(prev, v->u.var.vidx)->vd.kind;
    lua_assert(eqstr(name, getlocalvardesc(prev, v->u.var.vidx)->vd.name));
  }
  else {
    // ����㺯������ֵ
    // ����ֵ��Դ����㺯������ֵ
    up->instack = 0;
    // info��¼��Proto::upvalues�е��±�
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
// �鿴��ǰ�����㼶�Ƿ����и�NAME���еĻ��򷵻ض�Ӧλ�ã��Ҳ�������-1
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
// ��Ǳ����������ڵĿ飨block�����Ա��ں������ɹر�ָ��ʱ�ܹ���ȷ������ֵ��upvalue��
// fs��ǰ������״̬��Ϣ
// levelҪ��ǵı������ڵ�������㼶��
// fs->ls->dyd->actvar.arr[fs->firstlocal + vidx]���������vidx
static void markupval (FuncState *fs, int level) {
  BlockCnt *bl = fs->bl;
  // �����������ҵ���������Ŀ�
  // bl->nactvar�ڽ��뱾block��ʱ�������Ծ�ľֲ�����������
  // bl->nactvar > level��ʾ��ǰ����ⲿ��Ծ�ֲ�������������Ŀ������Ĳ㼶
  while (bl->nactvar > level)
    bl = bl->previous;
  // ��Ŀ����upval���Ϊ1����ʾ�ÿ����ĳЩ�������������upvalues
  bl->upval = 1;
  // level��Ӧ�ı������ڵĺ���needclose���Ϊ1��
  // ��������ʱ�����������ɹر�ָ�ȷ���������������ڵõ���ȷ����
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
//  �ں����㼶fs�в�����Ϊn�ı��������var��baseΪ1��ʾ��ǰ�����㼶
static void singlevaraux (FuncState *fs, TString *n, expdesc *var, int base) {
  if (fs == NULL)  /* no more levels? */
    // �����ǰ�����㼶ΪNULL����˵����������ˣ����ű�������ô��NAMEһ����ȫ�֣�����init_exp()��ʼ��VVOID������
    init_exp(var, VVOID, 0);  /* default is global */
  else {
    // �����㼶��ΪNULL�������searchvar()�鿴��ǰ�㼶�Ƿ����и�NAME���еĻ��򷵻ض�Ӧλ�ã��Ҳ�������-1
    int v = searchvar(fs, n, var);  /* look up locals at current level */
    if (v >= 0) {  /* found? */
      // �ҵ��˲��Ҳ��ǵ�һ�㺯����˵���˾������ϲ㺯�����ҵ��ˣ�����Ϊ��ֵ
      if (v == VLOCAL && !base)
        // ���Ϊ��ֵ
        markupval(fs, var->u.var.vidx);  /* local will be used as an upval */
    }
    else {  /* not found as local at current level; try upvalues */
      // �Ҳ�����������ֵ�в���
      int idx = searchupvalue(fs, n);  /* try existing upvalues */
      if (idx < 0) {  /* not found? */
        // ��ֵ���Ҳ�����ֻ��ȥ�ϲ㺯�����ˣ�ע�������base����Ϊ0
        singlevaraux(fs->prev, n, var, 0);  /* try upper levels */
        if (var->k == VLOCAL || var->k == VUPVAL)  /* local or upvalue? */
          // ������ϲ㺯���еľֲ�����������ֵ���ͼ��뵽�������ֵ��
          idx  = newupvalue(fs, n, var);  /* will be a new upvalue */
        else  /* it is a global or a constant */
          return;  /* don't need to do anything at this level */
      }
      // �ҵ���Ϊ��ֵ����
      init_exp(var, VUPVAL, idx);  /* new or old upvalue */
    }
  }
}


/*
** Find a variable with the given name 'n', handling global variables
** too.
*/
// ���ұ������ӵ�ǰ�����㼶local���ң��Ҳ�����ǰ�㼶upvalue���ң�
// ����һ�������㼶����local���ң��Ҳ�������һ�������㼶upvalue���ң�
// ���ջ���ȫ�ֲ���
// ע�⣺�������һ�������㼶�ҵ�������뵽����upvalue��
static void singlevar (LexState *ls, expdesc *var) {
  // �õ���������
  TString *varname = str_checkname(ls);
  FuncState *fs = ls->fs;
  // �ڵ�ǰ�����㼶����
  singlevaraux(fs, varname, var, 1);
  if (var->k == VVOID) {  /* global name? */
    // ˵��Ҫ��ȫ�ֱ���������_ENV
    // var���ҵ���ֵ��_ENV���ʽ����
    // key�Ǹñ������Ƶı��ʽ����
    expdesc key;
    // ����_ENV
    singlevaraux(fs, ls->envn, var, 1);  /* get environment variable */
    // �϶������ҵ���_ENV��ÿ��������һ����ֵ
    lua_assert(var->k != VVOID);  /* this one must exist */
    // ȷ��_ENV�Ľ���洢�ڼĴ�������ֵ��
    // ˵���������޸���Closure�Ļ�������������Ҫȷ���ǼĴ���������ֵ
    luaK_exp2anyregup(fs, var);  /* but could be a constant */
    // ����key�ַ������������ͱ��ʽ
    codestring(&key, varname);  /* key is variable name */
    // �������ʽt[k]��var��
    luaK_indexed(fs, var, &key);  /* env[varname] */
  }
}


/*
** Adjust the number of results from an expression list 'e' with 'nexps'
** expressions to 'nvars' values.
*/
// ��ֵ�������ʽ�б�e�Ľ��������nexps�����ʽ����Ϊnvars��ֵ
// �����ɻ��ߵ������е�ָ��
// nvars������������
// nexps�Ҳ���ʽ������
// e�Ҳ����һ�����ʽ������
static void adjust_assign (LexState *ls, int nvars, int nexps, expdesc *e) {
  FuncState *fs = ls->fs;
  // �������������Ҳ���ʽ����֮��Ĳ�ֵ����Ҫ���ⲹ��
  int needed = nvars - nexps;  /* extra values needed */
  // ������һ�����ʽ�Ƿ��Ƕ��ط���ֵ
  if (hasmultret(e->k)) {  /* last expression has multiple returns? */
    // ���һ���Ҳ���ʽ�Ƕ෵��ֵ��������䷵��ֵ������ƥ������
    // +1���ų����һ�����ʽ���� 
    int extra = needed + 1;  /* discount last expression itself */
    if (extra < 0)
      extra = 0;
    // ȷ���෵��ֵ���ʽ�ṩ����Ķ���ֵ
    luaK_setreturns(fs, e, extra);  /* last exp. provides the difference */
  }
  else {
    if (e->k != VVOID)  /* at least one expression? */
      // �Ҳ����һ�����ʽҲ��Ҫȷ���ڼĴ�����
      luaK_exp2nextreg(fs, e);  /* close last expression */
    if (needed > 0)  /* missing values? */
      // ����OP_LOADNILָ�fs->freereg��ǰ���üĴ����±꣬needed������Ҫдnil����
      luaK_nil(fs, fs->freereg, needed);  /* complete with nils */
  }
  if (needed > 0)
    // 1.���һ�����ʽ�Ƕ෵��ֵ���϶������OP_VCALLָ�����OP_VVARARGָ�����������ͬ����ҪԤ���Ĵ���
    // 2.����OP_LOADNIL������nil��������������Ҳ��Ҫ�ѼĴ���Ԥ������
    luaK_reserveregs(fs, needed);  /* registers for extra values */
  else  /* adding 'needed' is actually a subtraction */
    fs->freereg += needed;  /* remove extra values */
}


// ����һ���µ�block 
#define enterlevel(ls)	luaE_incCstack(ls->L)


// �뿪block
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
// g��ƥ��ı�ǩ��ls->dyd->gt�б��е�����
// label��������Ҫƥ��ı�ǩ
static void solvegoto (LexState *ls, int g, Labeldesc *label) {
  int i;
  // �ȴ�ƥ��ı�ǩ��Ϣ����
  Labellist *gl = &ls->dyd->gt;  /* list of gotos */
  // ȡ����δƥ��ı�ǩ
  Labeldesc *gt = &gl->arr[g];  /* goto to be resolved */
  lua_assert(eqstr(gt->name, label->name));
  // ���δƥ��ı�ǩ(goto/break)�Ƿ��Խ�������������
  if (l_unlikely(gt->nactvar < label->nactvar))  /* enter some scope? */
    jumpscopeerror(ls, gt);
  // ����ƥ���ǩ����תָ���޲�ΪĿ���ǩ��ָ���ַ
  luaK_patchlist(ls->fs, gt->pc, label->pc);
  // ʹ��ѭ��������δƥ���ǩ��ǰ�ƶ�һλ�����ǵ���ƥ��ı�ǩ
  for (i = g; i < gl->n - 1; i++)  /* remove goto from pending list */
    gl->arr[i] = gl->arr[i + 1];
  gl->n--;
}


/*
** Search for an active label with the given name.
*/
// ���Ѿ��������б���ұ�ǩ
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
// �ڶ�Ӧ�б������label����goto
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


// �ڶ�Ӧδƥ���б������label����goto
static int newgotoentry (LexState *ls, TString *name, int line, int pc) {
  return newlabelentry(ls, &ls->dyd->gt, name, line, pc);
}


/*
** Solves forward jumps. Check whether new label 'lb' matches any
** pending gotos in current block and solves them. Return true
** if any of the gotos need to close upvalues.
*/
// ����ǰ����ת
static int solvegotos (LexState *ls, Labeldesc *lb) {
  Labellist *gl = &ls->dyd->gt;
  int i = ls->fs->bl->firstgoto;
  // �Ƿ�ر���ֵ
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
// ����ָ�����Ƶ��±�ǩ
// ls�ʷ�״̬��
// name��ǩ����
// line��ǩ�����к�
// last����ָʾ�ñ�ǩ�Ƿ��������ڿ��е����һ���ǲ�����䣬����label�����ǲ��Ǿ��ǿ������
// labelh��;����Ϊ��non-op statement
static int createlabel (LexState *ls, TString *name, int line,
                        int last) {
  FuncState *fs = ls->fs;
  Labellist *ll = &ls->dyd->label;
  // ��ӵ��Ѿ������ı�ǩ������
  int l = newlabelentry(ls, ll, name, line, luaK_getlabel(fs));
  if (last) {  /* label is last no-op statement in the block? */
    // ��ǩ�ǿ��е����һ���ǲ������
    // ���±�ǩ�Ļ����������nactvar������ȷ���ֲ���������������ȷ
    /* assume that locals are already out of scope */
    // newlabelentry��������Ѿ���Ϊls->fs->nactvar�����﷢�ֺ�����ǿ����������Ϊ
    // ���������֮ǰ�ı��ر����ĸ���
    ll->arr[l].nactvar = fs->bl->nactvar;
  }
  // ������д������goto
  if (solvegotos(ls, &ll->arr[l])) {  /* need close? */
    luaK_codeABC(fs, OP_CLOSE, luaY_nvarstack(fs), 0, 0);
    return 1;
  }
  return 0;
}


/*
** Adjust pending gotos to outer level of a block.
*/
// ����ƥ��ı�ǩ����������ⲿ����
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


// ����block����ʼ����Ϣ
static void enterblock (FuncState *fs, BlockCnt *bl, lu_byte isloop) {
  bl->isloop = isloop;
  bl->nactvar = fs->nactvar;
  // ��ǰ���ÿ���λ��
  bl->firstlabel = fs->ls->dyd->label.n;
  // ��ǰ���ÿ���λ��
  bl->firstgoto = fs->ls->dyd->gt.n;
  bl->upval = 0;
  bl->insidetbc = (fs->bl != NULL && fs->bl->insidetbc);
  // ��������
  bl->previous = fs->bl;
  // ���µ��µ�block
  fs->bl = bl;
  lua_assert(fs->freereg == luaY_nvarstack(fs));
}


/*
** generates an error for an undefined 'goto'.
*/
// ����������⵽һ��δƥ���goto���߷Ƿ���breakʱ��������Ӧ�Ĵ�����Ϣ����ֹ�������
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


// �뿪block
static void leaveblock (FuncState *fs) {
  // ��ǰblock��Ϣ
  BlockCnt *bl = fs->bl;
  // �ʷ�״̬��
  LexState *ls = fs->ls;
  // ��־�Ƿ���Ҫ����OP_CLOSEָ��
  int hasclose = 0;
  // ���㵱ǰ����ļĴ����㼶���Ա����˳���ʱ�ָ����ɼĴ�����������freereg��
  // ��ʵ������һ�����õļĴ����㼶
  int stklevel = reglevel(fs, bl->nactvar);  /* level outside the block */
  // ɾ����ǰ������ľֲ���������һ��������ı��������һ���µ�����Ϣ(endpc)
  removevars(fs, bl->nactvar);  /* remove block locals */
  lua_assert(bl->nactvar == fs->nactvar);  /* back to level on entry */
  if (bl->isloop)  /* has to fix pending breaks? */
    hasclose = createlabel(ls, luaS_newliteral(ls->L, "break"), 0, 0);
  if (!hasclose && bl->previous && bl->upval)  /* still need a 'close'? */
    luaK_codeABC(fs, OP_CLOSE, stklevel, 0, 0);
  // �ָ����е�ջ�±�
  fs->freereg = stklevel;  /* free registers */
  // Ҫ�ر��������lable���Ƴ�
  ls->dyd->label.n = bl->firstlabel;  /* remove local labels */
  // ����ǰ���л������飨bl->previous�����Ա��������Ƕ�׿���ⲿ���� 
  fs->bl = bl->previous;  /* current block now is previous one */
  // �Ƿ���Ƕ�׿�
  if (bl->previous)  /* was it a nested block? */
    // ��ǰ�������д���δƥ��ı�ǩ��û�ڵ�ǰ�������ҵ���������Ҫ����Щδƥ��ı�ǩ�ƶ�����һ��������������ұ�ǩ
    movegotosout(fs, bl);  /* update pending gotos to enclosing block */
  else {
    // �Ѿ�������������bl->firstgoto < ls->dyd->gt.n��˵����ǰ�����������δƥ��ı�ǩ
    // �����ܴ��ڴ��ڣ������ڣ����ھ�˵��û��δƥ��ı�ǩ
    if (bl->firstgoto < ls->dyd->gt.n)  /* still pending gotos? */
      undefgoto(ls, &ls->dyd->gt.arr[bl->firstgoto]);  /* error */
  }
}


/*
** adds a new prototype into list of prototypes
*/
// �����µĺ���ԭ�ͣ���ӵ�����ԭ������
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
// �������ɴ����հ����ֽ���ָ��
// ������ҪĿ����Ϊ�հ�����Ĵ�������ȷ��GC�ܹ���ȷʶ����Щ�Ĵ�������ʹ��
static void codeclosure (LexState *ls, expdesc *v) {
  // �õ�������
  FuncState *fs = ls->fs->prev;
  // AΪ0������ͻ����
  init_exp(v, VRELOC, luaK_codeABx(fs, OP_CLOSURE, 0, fs->np - 1));
  // ���հ��Ľ���̶��ڵ�ǰ���õ����һ���Ĵ�����
  // ����Ϊ��ȷ��GC�ܹ���ȷʶ����Щ�Ĵ�������ʹ��
  luaK_exp2nextreg(fs, v);  /* fix it at the last register */
}


// �򿪺�������ʼ��fs
// fs�µģ�bl�µ�
static void open_func (LexState *ls, FuncState *fs, BlockCnt *bl) {
  Proto *f = fs->f;
  // ��������
  fs->prev = ls->fs;  /* linked list of funcstates */
  fs->ls = ls;
  // �л������ں���
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
  // ��ǰ���ÿ���λ��
  fs->firstlocal = ls->dyd->actvar.n;
  // ��ǰ���ÿ���λ��
  fs->firstlabel = ls->dyd->label.n;
  fs->bl = NULL;
  f->source = ls->source;
  luaC_objbarrier(ls->L, f, f->source);
  f->maxstacksize = 2;  /* registers 0/1 are always valid */
  enterblock(fs, bl, 0);
}


// ������������ˣ��رպ���
static void close_func (LexState *ls) {
  lua_State *L = ls->L;
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  // ����һ�����շ���ָ��²��Ƕ����õģ��Ͼ��е�û����ʾдreturn
  // ����luaY_nvarstack(fs)��ʾ����ֵ����ʼ�Ĵ�������
  // ����0��ʾ�������ֵ
  luaK_ret(fs, luaY_nvarstack(fs), 0);  /* final return */
  // �뿪block
  leaveblock(fs);
  lua_assert(fs->bl == NULL);
  // �������ɵ�ָ������Ż��͵�
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
// ��ǰtoken�Ƿ���block������ǣ�����TK_UNTIL�������д�����Ϊ
// �����ر�������
static int block_follow (LexState *ls, int withuntil) {
  switch (ls->t.token) {
    case TK_ELSE: case TK_ELSEIF:
    case TK_END: case TK_EOS:
      return 1;
    case TK_UNTIL: return withuntil;
    default: return 0;
  }
}


// ����statlist
/* statlist -> { stat [';'] } */
// 
// chunk :: = block
// block :: = { stat }[retstat]
// retstat ::= return [explist] [��;��]
static void statlist (LexState *ls) {
  /* statlist -> { stat [';'] } */
  // ��ǰtokenֻҪ����block������ǣ��ͼ���������ֱ������block����
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


// ����
// index -> '[' expr ']'
static void yindex (LexState *ls, expdesc *v) {
  /* index -> '[' expr ']' */
  luaX_next(ls);  /* skip the '[' */
  expr(ls, v);
  // ȷ�����ʽ�Ľ���洢�ڼĴ����л���һ������
  // Ϊ�˴����ӵ�kye�ɣ����磺t[a and b or c]
  luaK_exp2val(ls->fs, v);
  checknext(ls, ']');
}


/*
** {======================================================================
** Rules for Constructors
** =======================================================================
*/


// �����Ĺ������
typedef struct ConsControl {
  // �ڱ�������У����鲿�ֶ��ᱻ����Ϊһ��expdesc�ṹ��
  // ������ʱ�洢��ǰ�������
  expdesc v;  /* last list item read */
  // ��ı��ʽ����
  expdesc *t;  /* table descriptor */
  // ��ϣ���ֵ��ֶ�����
  int nh;  /* total number of 'record' elements */
  // ���鲿�ֵ��ֶ�����
  int na;  /* number of array elements already stored */
  // ���洢���ֶ�����
  int tostore;  /* number of array elements pending to be stored */
} ConsControl;


// ����
// recfield -> (NAME | '['exp']') = exp
static void recfield (LexState *ls, ConsControl *cc) {
  /* recfield -> (NAME | '['exp']') = exp */
  FuncState *fs = ls->fs;
  // ��ǰ���мĴ�������ʼλ��
  int reg = ls->fs->freereg;
  // ���Ŀ��Ĵ��������ı��ʽ������Ϣ��ֵ�ı��ʽ������Ϣ
  expdesc tab, key, val;
  if (ls->t.token == TK_NAME) {
    // ����NAME
    checklimit(fs, cc->nh, MAX_INT, "items in a constructor");
    // ����ʶ��ת��Ϊ���ı��ʽ
    codename(ls, &key);
  }
  else  /* ls->t.token == '[' */
    // ����[exp]��ʽ�ļ�
    yindex(ls, &key);
  cc->nh++;
  checknext(ls, '=');
  tab = *cc->t;
  // ���ɱ���������tab[key]���ʽ��tab
  luaK_indexed(fs, &tab, &key);
  // ���� = exp
  expr(ls, &val);
  // valд�뵽tab
  luaK_storevar(fs, &tab, &val);
  // ���ͷ��ͷ�
  fs->freereg = reg;  /* free registers */
}


// ����ǰ�������ı��ʽֵ��ӵ������������鲿�֣����ڱ�Ҫʱˢ�»������������ֽ���
static void closelistfield (FuncState *fs, ConsControl *cc) {
  // ��ʾû����Ч���б��ֶΣ�ֱ�ӷ���
  if (cc->v.k == VVOID) return;  /* there is no list item */
  // ȷ���ڼĴ�����
  luaK_exp2nextreg(fs, &cc->v);
  // ����Ϊ VVOID����ʾ��ǰ���ʽֵ�Ѿ�������
  cc->v.k = VVOID;
  // ����������
  if (cc->tostore == LFIELDS_PER_FLUSH) {
    // 
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);  /* flush */
    cc->na += cc->tostore;
    cc->tostore = 0;  /* no more items pending */
  }
}


// �������ֶλ�������ʣ���ֶμ������
static void lastlistfield (FuncState *fs, ConsControl *cc) {
  if (cc->tostore == 0) return;
  if (hasmultret(cc->v.k)) {
    // �����ǰ�ֶ���һ���෵��ֵ���ʽ
    // 
    luaK_setmultret(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, LUA_MULTRET);
    cc->na--;  /* do not count last expression (unknown number of elements) */
  }
  else {
    // ���Ƕ෵��ֵ���ʽ
    if (cc->v.k != VVOID)
      // �������֣�{1,2,a=1,b=2,3}��3������Ӧ����Ҫд��Ĵ�����
      luaK_exp2nextreg(fs, &cc->v);
    luaK_setlist(fs, cc->t->u.info, cc->na, cc->tostore);
  }
  cc->na += cc->tostore;
}


// ����
// listfield -> exp
static void listfield (LexState *ls, ConsControl *cc) {
  /* listfield -> exp */
  expr(ls, &cc->v);
  cc->tostore++;
}


// ����
// field -> listfield | recfield
static void field (LexState *ls, ConsControl *cc) {
  /* field -> listfield | recfield */
  switch(ls->t.token) {
    case TK_NAME: {  /* may be 'listfield' or 'recfield' */
      // �п����������ֶλ��ϣ�ֶ�
      if (luaX_lookahead(ls) != '=')  /* expression? */
        // �����ֶ�
        listfield(ls, cc);
      else
        // ��ϣ�ֶ�
        recfield(ls, cc);
      break;
    }
    case '[': {
      // ��ϣ�ֶ�
      recfield(ls, cc);
      break;
    }
    default: {
	  // �����ֶ�
      listfield(ls, cc);
      break;
    }
  }
}


// ����
// constructor -> '{' [ field { sep field } [sep] ] '}'
// sep -> ',' | ';'
static void constructor (LexState *ls, expdesc *t) {
  /* constructor -> '{' [ field { sep field } [sep] ] '}'
     sep -> ',' | ';' */
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  // ��δ��ʽָ���Ĵ��������������luaK_settablesize����OP_NEWTABLEָ��Ĳ���
  int pc = luaK_codeABC(fs, OP_NEWTABLE, 0, 0, 0);
  ConsControl cc;
  // Ԥ��һ�������ָ��ռ䣬���ں���������Ҫ�Ĳ�����OP_EXTRAARG׼��
  luaK_code(fs, 0);  /* space for extra arg. */
  cc.na = cc.nh = cc.tostore = 0;
  cc.t = t;
  // �������һ���Ĵ�����VNONRELOC
  init_exp(t, VNONRELOC, fs->freereg);  /* table will be at stack top */
  // ���涼�����ˣ�������Ҫ����Ų
  luaK_reserveregs(fs, 1);
  // ��ǰ��û�н������ֶ�
  init_exp(&cc.v, VVOID, 0);  /* no value (yet) */
  checknext(ls, '{');
  // ʹ��ѭ����������ֶ�
  do {
    lua_assert(cc.v.k == VVOID || cc.tostore > 0);
    if (ls->t.token == '}') break;
    // ���������ֶεĻ�����
    closelistfield(fs, &cc);
    // ���������ֶ�
    field(ls, &cc);
  } while (testnext(ls, ',') || testnext(ls, ';'));
  check_match(ls, '}', '{', line);
  // �������ֶλ�������ʣ���ֶμ������
  lastlistfield(fs, &cc);
  // ���ñ��С
  luaK_settablesize(fs, pc, t->u.info, cc.na, cc.nh);
}

/* }====================================================================== */


// ���ú����ɱ����������ָ��
static void setvararg (FuncState *fs, int nparams) {
  fs->f->is_vararg = 1;
  luaK_codeABC(fs, OP_VARARGPREP, nparams, 0, 0);
}


// ����
// parlist -> [ {NAME ','} (NAME | '...') ]
static void parlist (LexState *ls) {
  /* parlist -> [ {NAME ','} (NAME | '...') ] */
  FuncState *fs = ls->fs;
  Proto *f = fs->f;
  // �̶���������
  int nparams = 0;
  // �Ƿ��пɱ����
  int isvararg = 0;
  if (ls->t.token != ')') {  /* is 'parlist' not empty? */
    // ˵��������Ϊ��
    do {
      switch (ls->t.token) {
        case TK_NAME: {
          // �����ֲ�����
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
  // �ֲ�����д��������
  adjustlocalvars(ls, nparams);
  f->numparams = cast_byte(fs->nactvar);
  if (isvararg)
    // 
    setvararg(fs, f->numparams);  /* declared vararg */
  // Ԥ����Ĵ����ռ�
  luaK_reserveregs(fs, fs->nactvar);  /* reserve registers for parameters */
}


// ����������
// ��(�� [parlist] ��)�� block end
static void body (LexState *ls, expdesc *e, int ismethod, int line) {
  /* body ->  '(' parlist ')' block END */
  FuncState new_fs;
  BlockCnt bl;
  // �º�������proto
  new_fs.f = addprototype(ls);
  new_fs.f->linedefined = line;
  // ������block
  open_func(ls, &new_fs, &bl);
  checknext(ls, '(');
  if (ismethod) {
    new_localvarliteral(ls, "self");  /* create 'self' parameter */
    adjustlocalvars(ls, 1);
  }
  // ��������
  parlist(ls);
  checknext(ls, ')');
  // ����������
  statlist(ls);
  new_fs.f->lastlinedefined = ls->linenumber;
  check_match(ls, TK_END, TK_FUNCTION, line);
  // ���ɴ����հ����ֽ���ָ��
  codeclosure(ls, e);
  // ������������ˣ��رպ���
  close_func(ls);
}


// �������ʽ�б����ز�������
// explist ::= exp {��,�� exp}
static int explist (LexState *ls, expdesc *v) {
  /* explist -> expr { ',' expr } */
  // ��������������һ��
  int n = 1;  /* at least one expression */
  expr(ls, v);
  while (testnext(ls, ',')) {
    // ���滹Ҫ��v�أ�����������Ҫ�ŵ��Ĵ�����
    luaK_exp2nextreg(ls->fs, v);
    expr(ls, v);
    n++;
  }
  return n;
}


// ����
// funcargs ::= '(' [ explist ] ')' | constructor | STRING
static void funcargs (LexState *ls, expdesc *f) {
  FuncState *fs = ls->fs;
  // �����������ʽ
  expdesc args;
  // Ŀ�꺯���ڼĴ����е�λ�ã�����������
  int base, nparams;
  // ��ǰ�кţ����ڴ��󱨸�͵�����Ϣ
  int line = ls->linenumber;
  switch (ls->t.token) {
    case '(': {  /* funcargs -> '(' [ explist ] ')' */
      luaX_next(ls);
      if (ls->t.token == ')')  /* arg list is empty? */
        // �����б�Ϊ��
        args.k = VVOID;
      else {
        explist(ls, &args);
        // ������һ�����ʽ�Ƕ෵��ֵ��
        if (hasmultret(args.k))
          luaK_setmultret(fs, &args);
      }
      check_match(ls, ')', '(', line);
      break;
    }
    case '{': {  /* funcargs -> constructor */
      // ������һ��������
      constructor(ls, &args);
      break;
    }
    case TK_STRING: {  /* funcargs -> STRING */
      // �������ַ���������
      codestring(&args, ls->t.seminfo.ts);
      luaX_next(ls);  /* must use 'seminfo' before 'next' */
      break;
    }
    default: {
      luaX_syntaxerror(ls, "function arguments expected");
    }
  }
  lua_assert(f->k == VNONRELOC);
  // ���ú����ڼĴ�����λ��
  base = f->u.info;  /* base register for call */
  if (hasmultret(args.k))
    nparams = LUA_MULTRET;  /* open call */
  else {
    // ������ڲ��������ﴦ�����һ�������������һ�������̶����Ĵ���
    if (args.k != VVOID)
      luaK_exp2nextreg(fs, &args);  /* close last argument */
    // �������������fs->freereg-1�������һ��������λ�ã������1��ʾû����
    nparams = fs->freereg - (base+1);
  }
  // c=2��ʱ������һ������ֵ��
  init_exp(f, VCALL, luaK_codeABC(fs, OP_CALL, base, nparams+1, 2));
  // �����������ɵ��ֽ���ָ����Դ������к�line��������
  luaK_fixline(fs, line);
  // ����������ɺ�������������ᱻ��ջ���Ƴ�������ֻ��һ������ֵ����������ֻ����һ��
  fs->freereg = base+1;  /* call removes function and arguments and leaves
                            one result (unless changed later) */
}




/*
** {======================================================================
** Expression parsing
** =======================================================================
*/


// ����
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


// ����
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
        // ���ɱ���������t[k]���ʽ��v
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


// ����
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


// ����tokenId��ȡ��ǰһԪ����
static UnOpr getunopr (int op) {
  switch (op) {
    // not
    case TK_NOT: return OPR_NOT;
    // ����
    case '-': return OPR_MINUS;
    // ��
    case '~': return OPR_BNOT;
    // �󳤶�
    case '#': return OPR_LEN;
    // ����һԪ������
    default: return OPR_NOUNOPR;
  }
}


// ����tokenId��ȡ��ǰ��Ԫ����
static BinOpr getbinopr (int op) {
  switch (op) {
    // ��
    case '+': return OPR_ADD;
    // ��
    case '-': return OPR_SUB;
    // ��
    case '*': return OPR_MUL;
    // ģ
    case '%': return OPR_MOD;
    // ��
    case '^': return OPR_POW;
    // ��
    case '/': return OPR_DIV;
    // ����
    case TK_IDIV: return OPR_IDIV;
    // ��
    case '&': return OPR_BAND;
    // ��
    case '|': return OPR_BOR;
    // ���
    case '~': return OPR_BXOR;
    // ����
    case TK_SHL: return OPR_SHL;
    // ����
    case TK_SHR: return OPR_SHR;
    // ����
    case TK_CONCAT: return OPR_CONCAT;
    // ������
    case TK_NE: return OPR_NE;
    // ����
    case TK_EQ: return OPR_EQ;
    // С��
    case '<': return OPR_LT;
    // С�ڵ���
    case TK_LE: return OPR_LE;
    // ����
    case '>': return OPR_GT;
    // ���ڵ���
    case TK_GE: return OPR_GE;
    // and
    case TK_AND: return OPR_AND;
    // or
    case TK_OR: return OPR_OR;
    // ���Ƕ�Ԫ����
    default: return OPR_NOBINOPR;
  }
}


/*
** Priority table for binary operators.
*/
/*
* Lua ����������ȼ��ӵ͵������±���ʾ��
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
*     һԪ�������not   #     -     ~��
*     ^
* ͨ��������ʹ�����Ÿı���ʽ�����ȼ��������������'..'�������������'^'�����ҽ�ϵģ��������ж�Ԫ�������Ϊ���ϡ�
*/
// ��Ԫ����������ȼ���
static const struct {
  // �� left > right���ҽ��
  // �� left == right�����ϣ������������
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
// �������ʽ
// ����ֵΪ��һ��δ����Ķ�Ԫ�����
static BinOpr subexpr (LexState *ls, expdesc *v, int limit) {
  BinOpr op;
  UnOpr uop;
  // �Ե�ǰ������Ƚ��м�⣬����ݹ�̫�����ᱨ��
  // ���ʽ�������Ƕ�ף����� 1 + 2 * (3 - 4 / (5 + 6))�������½������ݹ���ù������ջ�����
  enterlevel(ls);
  uop = getunopr(ls->t.token);
  // �����Ƿ���һԪ����
  if (uop != OPR_NOUNOPR) {  /* prefix (unary) operator? */
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    subexpr(ls, v, UNARY_PRIORITY);
    // ����һԪ��������
    luaK_prefix(ls->fs, uop, v, line);
  }
  else simpleexp(ls, v);
  /* expand while operators have priorities higher than 'limit' */
  op = getbinopr(ls->t.token);
  // �Ƕ�Ԫ������������ȼ�����limit
  while (op != OPR_NOBINOPR && priority[op].left > limit) {
    // �Ҳ��ӱ��ʽ��������Ϣ
    expdesc v2;
    // �Ҳ���ʽ��Ӧ�Ķ�Ԫ�����
    BinOpr nextop;
    int line = ls->linenumber;
    luaX_next(ls);  /* skip operator */
    // �ݶ�Ԫ����������ͣ������������v
    luaK_infix(ls->fs, op, v);
    /* read sub-expression with higher priority */
    // �����Ҳ���ӱ��ʽ
    nextop = subexpr(ls, &v2, priority[op].right);
    // �����Ԫ��������ʽд��v
    luaK_posfix(ls->fs, op, v, &v2, line);
    op = nextop;
  }
  leavelevel(ls);
  return op;  /* return first untreated operator */
}


// �������ʽ
static void expr (LexState *ls, expdesc *v) {
  subexpr(ls, v, 0);
}

/* }==================================================================== */



/*
** {======================================================================
** Rules for Statements
** =======================================================================
*/


// ����block
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
// ����ֵ������������б�������ȫ�ֱ������ֲ���������ֵ������������
struct LHS_assign {
  // prev ��һ��ָ����һ��LHS_assign�ṹ���ָ�룬���ڹ�������
  // ��ָ��ǰ�����ڵ��ǰһ�������ڵ㣬�Ӷ��γ�һ����������
  struct LHS_assign *prev;
  // ȫ�֣����أ���ֵ������
  expdesc v;  /* variable (global, local, upvalue, or indexed) */
};


/*
** check whether, in an assignment to an upvalue/local variable, the
** upvalue/local variable is begin used in a previous assignment to a
** table. If so, save original upvalue/local value in a safe place and
** use this safe copy in the previous assignment.
*/
// ����ڶ��ظ�ֵ������Ƿ����Ǳ�ڵı�����ͻ
// lua���ظ�ֵ����ͬʱ�Զ���������и�ֵ�����ĳ��������������Ŀ������������Ҳ���ʽ�б�ʹ�ã����ܻᵼ���������Ϊ
// Ϊ��ȷ����ֵ�߼���ȷ���������Ὣԭʼ��t�洢��һ����ʱ�Ĵ����У���Ϊ����ȫ�����������岽�����£�
// ������ʱ�Ĵ��� ��
// ������Ԥ��һ���Ĵ������ڴ洢��ȫ������
// ����ԭʼֵ ��
// ���t�Ǿֲ�������������OP_MOVEָ���t��ֵ���Ƶ���ʱ�Ĵ�����
// ���t����ֵ��������OP_GETUPVALָ�����ֵ���ص���ʱ�Ĵ�����
static void check_conflict (LexState *ls, struct LHS_assign *lh, expdesc *v) {
  FuncState *fs = ls->fs;
  // ���ڴ洢��ȫ�����ļĴ�������
  int extra = fs->freereg;  /* eventual position to save local variable */
  // �Ƿ��ͻ���
  int conflict = 0;
  // �����������֮ǰ�ĸ�ֵ����
  for (; lh; lh = lh->prev) {  /* check all previous assignments */
    // �Ƿ��Ǳ�����
    if (vkisindexed(lh->v.k)) {  /* assignment to table field? */
      // �Ǳ��������������������ֵ�У�key�ڳ��������ַ�������
      // local t = {}; local function inner() t["1"], t = 20, {}; end
      if (lh->v.k == VINDEXUP) {  /* is table an upvalue? */
        // ��鵱ǰ����v�Ƿ�Ϊ��ֵ�������Ƿ�����ֶεı���ͬ
        if (v->k == VUPVAL && lh->v.u.ind.t == v->u.info) {
          // ȷʵ��ͻ
          conflict = 1;  /* table is the upvalue being assigned now */
          // t.k��t�Ǳ�k�ڳ��������ַ�����
          // ind.t = t �Ĵ����±�
          // ind.idx = k�ڳ������е��±�
          lh->v.k = VINDEXSTR;
          // �����ֶεı����Ϊ��ȫ�����ļĴ�������
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
      }
      else {  /* table is a register */
        // ��������϶����ڼĴ�������
        // ��ǰ�����Ǳ��ر��� && ���Ӧ�ļĴ����͵�ǰ�����ļĴ���������ͬ
        // local t = {}; local x = 10; t[x], t = x, {}
        if (v->k == VLOCAL && lh->v.u.ind.t == v->u.var.ridx) {
          conflict = 1;  /* table is the local being assigned now */
          // ���Ӧ�ļĴ����޸�Ϊ��ȫ�����ļĴ�������
          lh->v.u.ind.t = extra;  /* assignment will use safe copy */
        }
        /* is index the local being assigned? */
        // �Ǳ������洢�ڼĴ����� && ��ǰ�����Ǳ��ر��� && ��������key�͵�ǰ�����ļĴ���������ͬ
        // local t = {}; local x = 10; t[x], x = x, 20
        if (lh->v.k == VINDEXED && v->k == VLOCAL &&
            lh->v.u.ind.idx == v->u.var.ridx) {
          conflict = 1;
          // ��������key��Ӧ�ļĴ����޸�Ϊ��ȫ�����ļĴ�������
          lh->v.u.ind.idx = extra;  /* previous assignment will use safe copy */
        }
      }
    }
  }
  if (conflict) {
    /* copy upvalue/local value to a temporary (in position 'extra') */
    if (v->k == VLOCAL)
      // �ֲ�����
      // A B	R[A] := R[B]
      luaK_codeABC(fs, OP_MOVE, extra, v->u.var.ridx, 0);
    else
      // ��ֵ
      // A B	R[A] := UpValue[B]
      luaK_codeABC(fs, OP_GETUPVAL, extra, v->u.info, 0);
    // Ԥ���Ĵ����ռ�
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
// �����ͱ�����ظ�ֵ��䣨multiple assignment����
// ��Ҫ�����ǵݹ�ش���ֵ������ı���������ȷ���Ҳ���ʽ��������������������ƥ��
// lh��ǰ��ֵ���������������ڵ�
// nvars��ǰ������������
static void restassign (LexState *ls, struct LHS_assign *lh, int nvars) {
  // ���ڴ洢�Ҳ���ʽ����Ϣ������Ƕ��ظ�ֵ��䣬��Ȼ�����һ�����ʽ
  expdesc e;
  check_condition(ls, vkisvar(lh->v.k), "syntax error");
  // ȷ����������ֻ����
  check_readonly(ls, &lh->v);
  // �����ǰ�����Ƕ��ţ�,������ʾ���и���ı�����Ҫ����
  // ����suffixedexp������һ����������������ӵ�������
  if (testnext(ls, ',')) {  /* restassign -> ',' suffixedexp restassign */
    // ����һ���µ�LHS_assign�ڵ�nv�����������ӵ���ǰ�ڵ�lh
    struct LHS_assign nv;
    nv.prev = lh;
    // ����suffixedexp������һ��������������洢��nv.v��
    suffixedexp(ls, &nv.v);
    // ����±���������������
    if (!vkisindexed(nv.v.k))
      // �����һ�������͵�ǰ�����Ƿ���ڳ�ͻ
      check_conflict(ls, lh, &nv.v);
    enterlevel(ls);  /* control recursion depth */
    // �ݹ����restassign����ʣ��ı��������û��ʣ������ʹ�������ĸ�ֵ
    restassign(ls, &nv, nvars+1);
    leavelevel(ls);
  }
  else {  /* restassign -> '=' explist */
    int nexps;
    checknext(ls, '=');
    // ����explist�����Ҳ�ı��ʽ�б������ر��ʽ��������e���Ҳ����һ�����ʽ
    nexps = explist(ls, &e);
    // �������಻һ������ƽһ��
    if (nexps != nvars)
      // ������������������������ɻ��ߵ������е�ָ��
      adjust_assign(ls, nvars, nexps, &e);
    else {
      // ��������һ�����϶���Ҫȷ�����һ�����ʽ��һ����һֵ
      luaK_setoneret(ls->fs, &e);  /* close last expression */
      // �����ʽ��ֵ�洢����������
      luaK_storevar(ls->fs, &lh->v, &e);
      return;  /* avoid default */
    }
  }
  // 1.�Ҳ���ʽ�б�Ϊ�ջ����Ҳ���ʽ������������������
  // �϶�����adjust_assign���е�����ҲԤ����������Ĵ���������ֻ�Ǹ���������ֵ��ͬ���ǻ����Ҳ�������������������ֵ
  // 2.�����������
  // ����϶��ǻ����Ҳ�������������������ֵ��ls->fs->freereg-1�϶�����Ҫ����ำֵ�ļĴ���
  init_exp(&e, VNONRELOC, ls->fs->freereg-1);  /* default assignment */
  // �����ʽ��ֵ�洢����������
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


// ����goto
// goto Name
static void gotostat (LexState *ls) {
  FuncState *fs = ls->fs;
  int line = ls->linenumber;
  TString *name = str_checkname(ls);  /* label's name */
  // �鿴��ǩ�Ƿ��Ѿ�����
  Labeldesc *lb = findlabel(ls, name);
  if (lb == NULL)  /* no label? */
    /* forward jump; will be resolved when the label is declared */
    // ǰ����ת
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
// ��鵱ǰ���������Ƿ��Ѿ�����ͬ����ǩ
static void checkrepeated (LexState *ls, TString *name) {
  Labeldesc *lb = findlabel(ls, name);
  if (l_unlikely(lb != NULL)) {  /* already defined? */
    const char *msg = "label '%s' already defined on line %d";
    msg = luaO_pushfstring(ls->L, msg, getstr(name), lb->line);
    luaK_semerror(ls, msg);  /* error */
  }
}


// ����
// label -> '::' NAME '::'
static void labelstat (LexState *ls, TString *name, int line) {
  /* label -> '::' NAME '::' */
  checknext(ls, TK_DBCOLON);  /* skip double colon */
  while (ls->t.token == ';' || ls->t.token == TK_DBCOLON)
    // ::middle:: ; ::empty:: ;; �������
    statement(ls);  /* skip other no-op statements */
  // ��鵱ǰ���������Ƿ��Ѿ�����ͬ����ǩ
  checkrepeated(ls, name);  /* check for repeated labels */
  // ������ǩ��������ע�ᵽ��ǰ��������
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


// ����
// test_then_block -> [IF | ELSEIF] cond THEN block
// escapelist��ת�б����ڴ洢��Ҫ�����ķ�֧����תָ��
static void test_then_block (LexState *ls, int *escapelist) {
  /* test_then_block -> [IF | ELSEIF] cond THEN block */
  BlockCnt bl;
  FuncState *fs = ls->fs;
  // �������ʽ��������Ϣ
  expdesc v;
  // ����ת�б�
  int jf;  /* instruction to skip 'then' code (if condition is false) */
  luaX_next(ls);  /* skip IF or ELSEIF */
  // �����������ʽ
  expr(ls, &v);  /* read condition */
  checknext(ls, TK_THEN);
  if (ls->t.token == TK_BREAK) {  /* 'if x then break' ? */
    // ����if x then break
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
    // ȷ��������Ϊ��ʱ����then���ֵĴ��룬Ϊ���ʱ�������������Ϊ��ʱ����then��
    luaK_goiftrue(ls->fs, &v);  /* skip over block if condition is false */
    // ����then block
    enterblock(fs, &bl, 0);
    // ��¼����ת�б�
    jf = v.f;
  }
  // ����then����
  statlist(ls);  /* 'then' part */
  // �뿪then block
  leaveblock(fs);
  // ���������Ƿ���else/elseif���
  if (ls->t.token == TK_ELSE ||
      ls->t.token == TK_ELSEIF)  /* followed by 'else'/'elseif'? */
    // ����������else/elseif����
    // ����һ����תָ���������ӵ���ת�б�escapelist���� ����ȷ����THEN��ִ����ɺ��ܹ�����������ELSE��ELSEIF����
    // ����Ŀǰ���Կ϶���֪�������תָ���������ʲô����Ҫ�����޲�
    luaK_concat(fs, escapelist, luaK_jump(fs));  /* must jump over it */
  // �޲�����ļ���ת���������λ�ð�
  luaK_patchtohere(fs, jf);
}


// ����
// ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END
static void ifstat (LexState *ls, int line) {
  /* ifstat -> IF cond THEN block {ELSEIF cond THEN block} [ELSE block] END */
  FuncState *fs = ls->fs;
  // ����else/elseif����תָ��
  int escapelist = NO_JUMP;  /* exit list for finished parts */
  test_then_block(ls, &escapelist);  /* IF cond THEN block */
  while (ls->t.token == TK_ELSEIF)
    test_then_block(ls, &escapelist);  /* ELSEIF cond THEN block */
  if (testnext(ls, TK_ELSE))
    block(ls);  /* 'else' part */
  check_match(ls, TK_END, TK_IF, line);
  // ��escapelist�е�������תָ���޲���if����ĩβ
  luaK_patchtohere(fs, escapelist);  /* patch escape list to 'if' end */
}


// ����
// local function Name funcbody
static void localfunc (LexState *ls) {
  expdesc b;
  FuncState *fs = ls->fs;
  int fvar = fs->nactvar;  /* function's variable index */
  // ls->dyd�����µľֲ�����
  new_localvar(ls, str_checkname(ls));  /* new local variable */
  // ���������򣬸����������ֵ��var->vd.ridx��var->vd.pidx
  adjustlocalvars(ls, 1);  /* enter its scope */
  // ����funcbody
  body(ls, &b, 0, ls->linenumber);  /* function created in next register */
  /* debug information will only see the variable after this point! */
  localdebuginfo(fs, fvar)->startpc = fs->pc;
}


// ��ȡ�ֲ�����<>���������
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


// ���to-be-closed����������OP_TBCָ��
static void checktoclose (FuncState *fs, int level) {
  if (level != -1) {  /* is there a to-be-closed variable? */
    marktobeclosed(fs);
    luaK_codeABC(fs, OP_TBC, reglevel(fs, level), 0, 0);
  }
}


// ����
// stat -> LOCAL NAME ATTRIB { ',' NAME ATTRIB } ['=' explist]
static void localstat (LexState *ls) {
  /* stat -> LOCAL NAME ATTRIB { ',' NAME ATTRIB } ['=' explist] */
  FuncState *fs = ls->fs;
  // 
  int toclose = -1;  /* index of to-be-closed variable (if any) */
  // ���һ����������
  Vardesc *var;  /* last variable */
  // ��ǰ��������±����������
  int vidx, kind;  /* index and kind of last variable */
  // ����ı�������
  int nvars = 0;
  // ��ֵ���ʽ������
  int nexps;
  // explist�ı��ʽ����һ���������һ��
  expdesc e;
  do {
    // �ŵ�ls->dyd->actvar.arr�У������±�
    vidx = new_localvar(ls, str_checkname(ls));
    // ������û�ж�������
    kind = getlocalattribute(ls);
    // ��������
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
    // û���Ҳำֵ���ʽ
    e.k = VVOID;
    nexps = 0;
  }
  // ���һ������
  var = getlocalvardesc(fs, vidx);  /* get last variable */
  // ��������͸�ֵ���ʽ����һ�� &&
  // ���һ�������ǳ��� &&
  // ���ʽ��һ�������������Ż���var��>k��
  if (nvars == nexps &&  /* no adjustments? */
      var->vd.kind == RDKCONST &&  /* last variable is const? */
      luaK_exp2const(fs, &e, &var->k)) {  /* compile-time constant? */
    var->vd.kind = RDKCTC;  /* variable is a compile-time constant */
    // ��������϶�����Ҫ����Ĵ������������������ų��Լ���ֻע��ǰ��ı���
    adjustlocalvars(ls, nvars - 1);  /* exclude last variable */
    // ���ǵ�ǰ������ı���������Ҫ+1
    fs->nactvar++;  /* but count it */
  }
  else {
    // ����nvars��nexps�������ɻ��ߵ������е�ָ��
    adjust_assign(ls, nvars, nexps, &e);
    // ���������ľֲ�����ע�ᵽ��ǰ�����򣬲�Ϊ�����Ĵ�������
    adjustlocalvars(ls, nvars);
  }
  // ���to-be-closed����������OP_TBCָ��
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


// ����
// func | assignment
static void exprstat (LexState *ls) {
  /* stat -> func | assignment */
  FuncState *fs = ls->fs;
  // �洢��ำֵ���ʽ����Ϣ
  struct LHS_assign v;
  // �Ƚ������ĵ�һ�����ʽ
  suffixedexp(ls, &v.v);
  if (ls->t.token == '=' || ls->t.token == ',') { /* stat -> assignment ? */
    // ��ֵ���
    v.prev = NULL;
    // ������ظ�ֵ��1��ʾĿǰ��һ������������
    restassign(ls, &v, 1);
  }
  else {  /* stat -> func */
    // �����������
    Instruction *inst;
    check_condition(ls, v.v.k == VCALL, "syntax error");
    inst = &getinstruction(fs, &v.v);
    // ���°�c���ó�1��û�з���ֵ
    SETARG_C(*inst, 1);  /* call statement uses no results */
  }
}


// ����
// stat -> RETURN [explist] [';']
static void retstat (LexState *ls) {
  /* stat -> RETURN [explist] [';'] */
  FuncState *fs = ls->fs;
  // �洢����ֵ�ı��ʽ��Ϣ
  expdesc e;
  // ����ֵ����
  int nret;  /* number of values being returned */
  // ���ﷵ�ص��Ǳ��ر���������Ҳ���ǵ�һ������ֵλ��
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


// ����state
static void statement (LexState *ls) {
  int line = ls->linenumber;  /* may be needed for error messages */
  // �Ե�ǰ������Ƚ��м�⣬����ݹ�̫�����ᱨ��
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
  // ����ǰ�����ļĴ���ʹ���������Ϊ�ֲ�����������
  ls->fs->freereg = luaY_nvarstack(ls->fs);  /* free registers */
  leavelevel(ls);
}

/* }====================================================================== */


/*
** compiles the main function, which is a regular vararg function with an
** upvalue named LUA_ENV
*/
// ����������
static void mainfunc (LexState *ls, FuncState *fs) {
  // top-level function block
  BlockCnt bl;
  Upvaldesc *env;
  // ����top-level block
  open_func(ls, fs, &bl);
  // �������Ǳ�Σ���Ҫ����OP_VARARGPREP
  setvararg(fs, 0);  /* main function is always declared vararg */
  // һ��Lua�ļ��������ʱ����ȴ�����һ����㣨Top level����Lua�հ���
  // �ñհ�Ĭ�ϴ���һ��UpValue�����UpValue�ı�����Ϊ"_ENV"��
  // ��ָ��Lua�������ȫ�ֱ�������_G���������Ϊ_G��Ϊ��ǰLua�ļ��д�������л���(env)��
  // ��ʵ�ϣ�ÿһ��Lua�հ����ǵ�һ��UpValueֵ����_ENV��
  env = allocupvalue(fs);  /* ...set environment upvalue */
  env->instack = 1;
  env->idx = 0;
  env->kind = VDKREG;
  env->name = ls->envn;
  luaC_objbarrier(ls->L, fs->f, env->name);
  // ��ȡ��һ��token
  luaX_next(ls);  /* read first token */
  // �����﷨����������block
  statlist(ls);  /* parse main body */
  check(ls, TK_EOS);
  // �˳�top-level block
  close_func(ls);
}


// �ʷ��﷨����
LClosure *luaY_parser (lua_State *L, ZIO *z, Mbuffer *buff,
                       Dyndata *dyd, const char *name, int firstchar) {
  LexState lexstate;
  FuncState funcstate;
  // ����main�հ�
  LClosure *cl = luaF_newLclosure(L, 1);  /* create main closure */
  setclLvalue2s(L, L->top.p, cl);  /* anchor it (to avoid being collected) */
  luaD_inctop(L);
  // ��������������ұ�
  lexstate.h = luaH_new(L);  /* create table for scanner */
  sethvalue2s(L, L->top.p, lexstate.h);  /* anchor it */
  luaD_inctop(L);
  // ����main����ԭ��
  funcstate.f = cl->p = luaF_newproto(L);
  luaC_objbarrier(L, cl, cl->p);
  funcstate.f->source = luaS_new(L, name);  /* create and anchor TString */
  luaC_objbarrier(L, funcstate.f, funcstate.f->source);
  lexstate.buff = buff;
  lexstate.dyd = dyd;
  dyd->actvar.n = dyd->gt.n = dyd->label.n = 0;
  luaX_setinput(L, &lexstate, z, funcstate.f->source, firstchar);
  // ����������
  mainfunc(&lexstate, &funcstate);
  lua_assert(!funcstate.prev && funcstate.nups == 1 && !lexstate.fs);
  /* all scopes should be correctly finished */
  lua_assert(dyd->actvar.n == 0 && dyd->gt.n == 0 && dyd->label.n == 0);
  L->top.p--;  /* remove scanner's table */
  return cl;  /* closure is on the stack, too */
}

