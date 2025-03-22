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
// lua�����ֶ���
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


// tokenʵ����Ϣ
typedef union {
  lua_Number r;
  lua_Integer i;
  TString *ts;
} SemInfo;  /* semantics information */


// token
typedef struct Token {
  // ���Ͷ�Ӧ��ö��ֵ
  int token;
  // �洢����ֵ��token��ֵ
  SemInfo seminfo;
} Token;


/* state of the lexer plus state of the parser when shared by all
   functions */
// �ʷ�״̬��
typedef struct LexState {
  // ��ǰ��ȡ���ַ�
  int current;   /* current character (charint) */
  // ��ǰ��ȡ������
  int linenumber;  /* input line counter */
  // ��һ��token����
  int lastline;  /* line of last token 'consumed' */
  // ��ǰtoken
  Token t;  /* current token */
  // ��ǰ��ȡ��token
  Token lookahead;  /* look ahead token */
  // ��ǰ�����ĺ���
  struct FuncState *fs;  /* current function (parser) */
  // ��Ӧ��lua״̬��
  struct lua_State *L;
  // �ַ�����ȡ��
  ZIO *z;  /* input stream */
  // ��ʱ���������������token
  Mbuffer *buff;  /* buffer for tokens */
  // ����������ұ����泣�����ӿ����ʱ�ĳ������ң���ȻҲ�Ǳ��ⱻGC
  // h[k] = index(Proto::k)
  Table *h;  /* to avoid collection/reuse strings */
  // �﷨���������У����local������Ϣ
  struct Dyndata *dyd;  /* dynamic structures used by the parser */
  // ��ǰ��Դ����
  TString *source;  /* current source name */
  // ������������
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
