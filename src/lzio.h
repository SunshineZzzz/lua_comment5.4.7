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

// ���л����ַ��Ͷ�ȡ��û�о����
#define zgetc(z)  (((z)->n--)>0 ?  cast_uchar(*(z)->p++) : luaZ_fill(z))


// token����
typedef struct Mbuffer {
  // ����
  char *buffer;
  // ��ǰ�ַ���
  size_t n;
  // ����ش�С
  size_t buffsize;
} Mbuffer;

// ��ʼ��
#define luaZ_initbuffer(L, buff) ((buff)->buffer = NULL, (buff)->buffsize = 0)

// ���ػ���
#define luaZ_buffer(buff)	((buff)->buffer)
// ���ػ���ش�С
#define luaZ_sizebuffer(buff)	((buff)->buffsize)
// ���ص�ǰ�ַ���
#define luaZ_bufflen(buff)	((buff)->n)

// ȥ����ǰi���ַ�
#define luaZ_buffremove(buff,i)	((buff)->n -= (i))
// ����
#define luaZ_resetbuffer(buff) ((buff)->n = 0)


// ����
#define luaZ_resizebuffer(L, buff, size) \
	((buff)->buffer = luaM_reallocvchar(L, (buff)->buffer, \
				(buff)->buffsize, size), \
	(buff)->buffsize = size)

#define luaZ_freebuffer(L, buff)	luaZ_resizebuffer(L, buff, 0)


LUAI_FUNC void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader,
                                        void *data);
LUAI_FUNC size_t luaZ_read (ZIO* z, void *b, size_t n);	/* read next n bytes */



/* --------- Private Part ------------------ */

// �ַ�����ȡ��
struct Zio {
  // ʣ��ɶ��ַ�
  size_t n;  /* bytes still unread */
  // ��ǰ��ȡ����λ��
  const char *p;  /* current position in buffer */
  // ��ȡ����
  lua_Reader reader;  /* reader function */
  // ��ȡ�������û�����
  void *data;	/* additional data */
  // ״̬��
  lua_State *L;  /* Lua state (for reader) */
};


LUAI_FUNC int luaZ_fill (ZIO *z);

#endif
