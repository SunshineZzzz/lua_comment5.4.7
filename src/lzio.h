/*
** $Id: lzio.h $
** Buffered streams
** See Copyright Notice in lua.h
*/


#ifndef lzio_h
#define lzio_h

#include "lua.h"

#include "lmem.h"


#define EOZ	(-1)			/* end of stream */

typedef struct Zio ZIO;

// 还有缓存字符就读取，没有就填充
#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : luaZ_fill(z))


// token缓存
typedef struct Mbuffer {
  // 缓存
  char *buffer;
  // 当前字符数
  size_t n;
  // 缓存池大小
  size_t buffsize;
} Mbuffer;

// 初始化
#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

// 返回缓存
#define luaZ_buffer(buff)	((buff)->buffer)
// 返回缓存池大小
#define luaZ_sizebuffer(buff)	((buff)->buffsize)
// 返回当前字符数
#define luaZ_bufflen(buff)	((buff)->n)

// 去掉上前i个字符
#define luaZ_buffremove(buff,i)	((buff)->n -= (i))
// 重置
#define luaZ_resetbuffer(buff) ((buff)->n = 0)


// 扩容
#define luaZ_resizebuffer(L, buff, size) \
	((buff)->buffer = luaM_reallocvchar(L, (buff)->buffer, \
				(buff)->buffsize, size), \
	(buff)->buffsize = size)

#define luaZ_freebuffer(L, buff)	luaZ_resizebuffer(L, buff, 0)


LUAI_FUNC void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader,
                                        void *data);
LUAI_FUNC size_t luaZ_read (ZIO* z, void *b, size_t n);	/* read next n bytes */



/* --------- Private Part ------------------ */

// 字符流读取器
struct Zio {
  // 剩余可读字符
  size_t n;  /* bytes still unread */
  // 当前读取到的位置
  const char *p;  /* current position in buffer */
  // 读取函数
  lua_Reader reader;  /* reader function */
  // 读取函数的用户数据
  void *data;	/* additional data */
  // 状态机
  lua_State *L;  /* Lua state (for reader) */
};


LUAI_FUNC int luaZ_fill (ZIO *z);

#endif
