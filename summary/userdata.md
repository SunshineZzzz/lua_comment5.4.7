1. UserData: Full User Data，全量级的用户数据。通常是一块内存区域，可用于转换为特殊的数据结构，UserData内部数据的解析与设置由使用者自己进行实现，Lua并没有能力处理这份二进制内存，Lua只负责这块内存区域的内存引用与管理回收。

2. LightUserData: 轻量级用户数据，相比于上面的FullUserData，它是轻量的，它只是一个指针，在C语言中对应的类型为void*，占4或8个字节，Lua不会管理这个指针的内存，全由使用者自己管理。

3. c中定义
```C
/* Ensures that addresses after this type are always fully aligned. */
// 用户自定义值
typedef union UValue {
  TValue uv;
  LUAI_MAXALIGN;  /* ensures maximum alignment for udata bytes */
} UValue;


/*
** Header for userdata with user values;
** memory area follows the end of this structure.
*/
// full user data
typedef struct Udata {
  CommonHeader;
  // 用户自定义值长度
  unsigned short nuvalue;  /* number of user values */
  // 用户数据长度
  size_t len;  /* number of bytes */
  // 元表
  struct Table *metatable;
  // GC相关的链表
  GCObject *gclist;
  // 用户自定义值内容
  UValue uv[1];  /* user values */
} Udata;
// 紧跟着的内存:
// C语言结构体占用内存


/*
** Header for userdata with no user values. These userdata do not need
** to be gray during GC, and therefore do not need a 'gclist' field.
** To simplify, the code always use 'Udata' for both kinds of userdata,
** making sure it never accesses 'gclist' on userdata with no user values.
** This structure here is used only to compute the correct size for
** this representation. (The 'bindata' field in its end ensures correct
** alignment for binary data following this header.)
*/
// 没有用户自定义值的full user data
// 为什么没有gclist域：
// Header for userdata with no user values. These userdata do not need
// to be gray during GC, and therefore do not need a 'gclist' field.
// 不会直接去创建这个结构体对应的对象，用来做偏移
typedef struct Udata0 {
  CommonHeader;
  unsigned short nuvalue;  /* number of user values */
  size_t len;  /* number of bytes */
  struct Table *metatable;
  union {LUAI_MAXALIGN;} bindata;
} Udata0;
// 紧跟着的内存:
// C语言结构体占用内存
```

4. Udata和Udata0: 在大多数情况下，使用Udata都不需要用到UValue数组，因为C语言结构体也存在于Udata之中（末尾），在这个结构体中完全可以存储所需要的任何数据，不需要非得存储在UValue数组当中，所以可以把Udata直接理解为Udata0。

5. UserData删除: 当没有被引用的时候就会被清除，在C语言中不需要手动管理这部分内存，这里的引用指的是在Lua中的使用，在C语言中调用时并不算引用。

6. UserData需要元表的配合才完整。
```lua
local people = peopleMod.new()

people:setName("zzz")
local strName = people:getName()
print(strName)
```
```C++
struct people
{
	char name[128];
};
static int newPeople(lua_State* L)
{
	people* pPeople = (people*)lua_newuserdata(L, sizeof(people));
	memset(pPeople->name, 0, 128);

	luaL_getmetatable(L, "peopleMT");
	lua_setmetatable(L, -2);

	return 1;
}
static int GetName(lua_State* L)
{
	people* pPeople = (people*)luaL_checkudata(L, 1, "peopleMT");
	lua_pushstring(L, pPeople->name);
	return 1;
}
static int SetName(lua_State* L)
{
	people* pPeople = (people*)luaL_checkudata(L, 1, "peopleMT");

	const char* pNewName = luaL_checkstring(L, 2);
	if (pNewName == NULL) 
	{
		return 0;
	}
	strncpy_s(pPeople->name, pNewName, 128);
	
	return 0;
}
static luaL_Reg people_mts[] =
{
	{ "getName", GetName },
	{ "setName", SetName },
	{ NULL, NULL }
};
static luaL_Reg people_mods[] =
{
	{ "new", newPeople},
	{ NULL, NULL }
};
int userdata()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	// 创建一个新的元表，名称为PopleMT，这样子后面就可以判断获取的内存对象是否有对应名称的原表
	// 这个元表会保存在全局表中
	luaL_newmetatable(L, "peopleMT");
	lua_pushvalue(L, -1);
	// 元表.__index = 元表
	lua_setfield(L, -2, "__index");
	// 将数组people_mts中的所有函数注册到peopleMT中
	luaL_setfuncs(L, people_mts, 0);
	lua_pop(L, 1);

	// 创建表，people_mods注册到其中
	luaL_newlib(L, people_mods);
	lua_pushvalue(L, -1);
	// 注册到全局表中
	lua_setglobal(L, "peopleMod");
	lua_pop(L, 1);

	if (luaL_dofile(L, "userdata.lua") != LUA_OK) 
	{
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}
```

7. light userdata例子
```C++

```