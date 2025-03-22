/*
** $Id: llex.h $
** Lexical Analyzer
** See Copyright Notice in lua.h
*/

#ifndef llex_h
#define llex_h

#include <limits.h>

#include "lobject.h"
#include "lzio.h"


/*
** Single-char tokens (terminal symbols) are represented by their own
** numeric code. Other tokens start at the following value.
*/
#define FIRST_RESERVED	(UCHAR_MAX + 1)


#if !defined(LUA_ENV)
#define LUA_ENV		"_ENV"
#endif


/*
* WARNING: if you change the order of this enumeration,
* grep "ORDER RESERVED"
*/
// lua保留字定义
enum RESERVED {
  /* terminal symbols denoted by reserved words */
  TK_AND = FIRST_RESERVED, TK_BREAK,
  TK_DO, TK_ELSE, TK_ELSEIF, TK_END, TK_FALSE, TK_FOR, TK_FUNCTION,
  TK_GOTO, TK_IF, TK_IN, TK_LOCAL, TK_NIL, TK_NOT, TK_OR, TK_REPEAT,
  TK_RETURN, TK_THEN, TK_TRUE, TK_UNTIL, TK_WHILE,
  /* other terminal symbols */
  TK_IDIV, TK_CONCAT, TK_DOTS, TK_EQ, TK_GE, TK_LE, TK_NE,
  TK_SHL, TK_SHR,
  TK_DBCOLON, TK_EOS,
  TK_FLT, TK_INT, TK_NAME, TK_STRING
};

/* number of reserved words */
#define NUM_RESERVED	(cast_int(TK_WHILE-FIRST_RESERVED + 1))


// token实际信息
typedef union {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} SemInfo;  /* semantics information */


// token
typedef struct Token {
  // 类型对应的枚举值
  int token;
  // 存储含有值的token的值
  SemInfo seminfo;
} Token;


/* state of the lexer plus state of the parser when shared by all
   functions */
// 词法状态机
typedef struct LexState {
  // 当前读取的字符
  int current;   /* current character (charint) */
  // 当前读取到的行
  int linenumber;  /* input line counter */
  // 上一个token的行
  int lastline;  /* line of last token 'consumed' */
  // 当前token
  Token t;  /* current token */
  // 提前获取的token
  Token lookahead;  /* look ahead token */
  // 当前解析的函数
  struct FuncState *fs;  /* current function (parser) */
  // 对应的lua状态机
  struct lua_State *L;
  // 字符流读取器
  ZIO *z;  /* input stream */
  // 临时缓冲区，存放完整token
  Mbuffer *buff;  /* buffer for tokens */
  // 常量缓存查找表，缓存常量，加快编译时的常量查找，当然也是避免被GC
  // h[k] = index(Proto::k)
  Table *h;  /* to avoid collection/reuse strings */
  // 语法分析过程中，存放local变量信息
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  // 当前资源名称
  TString *source;  /* current source name */
  // 环境变量名称
  TString *envn;  /* environment variable name */
} LexState;


LUAI_FUNC void luaX_init (lua_State *L);
LUAI_FUNC void luaX_setinput (lua_State *L, LexState *ls, ZIO *z,
                              TString *source, int firstchar);
LUAI_FUNC TString *luaX_newstring (LexState *ls, const char *str, size_t l);
LUAI_FUNC void luaX_next (LexState *ls);
LUAI_FUNC int luaX_lookahead (LexState *ls);
LUAI_FUNC l_noret luaX_syntaxerror (LexState *ls, const char *s);
LUAI_FUNC const char *luaX_token2str (LexState *ls, int token);


#endif
