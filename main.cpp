#include <iostream>
#include "./src/lua.hpp"

// 脚本运行基础总览
bool overview()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	if (luaL_dofile(L, "overview.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}

/////////////////////////////////////////////////////////////////////////////////

// c调用lua
bool ccalllua()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	if (luaL_dofile(L, "ccalllua.lua") != LUA_OK)
	{
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	if (lua_getfield(L, 1, "Add") != LUA_TFUNCTION)
	{
		fprintf(stderr, "get M.Add lua closure error\n");
		lua_close(L);
		return false;
	}
	lua_pushinteger(L, 1);
	lua_pushinteger(L, 2);
	// 2个参数，1个返回值
	lua_call(L, 2, 1);
	auto sum = lua_tointeger(L, -1);
	fprintf(stdout, "M.Add(%d,%d) = %d\n", 1, 2, (int)sum);
	lua_pop(L, 1);
	lua_pop(L, 1);

	lua_getglobal(L, "Add");
	lua_pushinteger(L, 1);
	lua_pushinteger(L, 2);
	lua_call(L, 2, 1);
	sum = lua_tointeger(L, -1);
	fprintf(stdout, "M.Add(%d,%d) = %d\n", 1, 2, (int)sum);
	lua_pop(L, 1);

	lua_close(L);
	return true;
}


/////////////////////////////////////////////////////////////////////////////////

// lua调用c
// 暴露给lua的函数必须满足如下声明，int表示返回值个数
// typedef int (*lua_CFunction)(lua_State* L)
static int lua_cfunc_print(lua_State* L) 
{
	int n = lua_gettop(L);
	int i;
	for (i = 1; i <= n; i++) 
	{
		size_t l;
		const char* s = luaL_tolstring(L, i, &l);
		if (i > 1)
			lua_writestring("\t", 1);
		lua_writestring(s, l);
		lua_pop(L, 1);
	}
	lua_writeline();
	return 0;
}
bool luacallc()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	// 全局表压入栈
	lua_pushglobaltable(L);
	// 把lua_cfunc_print中函数和名称压入全局表中
	lua_pushcfunction(L, lua_cfunc_print);
	lua_setfield(L, -2, "lua_cfunc_print");
	lua_pop(L, 1);

	if (luaL_dofile(L, "luacallc.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}

/////////////////////////////////////////////////////////////////////////////////

// 元表
bool metatable() 
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	if (luaL_dofile(L, "metatable.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}

/////////////////////////////////////////////////////////////////////////////////

// userdata
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
bool userdata()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	// 创建一个新的元表，名称为PopleMT，这样子后面就可以判断获取的内存对象是否有对应名称的原表
	// 这个元表会保存在全局表中
	luaL_newmetatable(L, "peopleMT");
	lua_pushvalue(L, -1);
	// 元表.__index = 元表
	lua_setfield(L, -2, "__index");
	// 元表中设置其他key<=>value
	luaL_setfuncs(L, people_mts, 0);
	lua_pop(L, 1);

	// 全局表加入peopleMod
	luaL_newlib(L, people_mods);
	lua_pushvalue(L, -1);
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

/////////////////////////////////////////////////////////////////////////////////

bool coroutine()
{
	lua_State* L = luaL_newstate();
	luaL_openlibs(L);

	if (luaL_dofile(L, "coroutine.lua") != LUA_OK)
	{
		fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}

int main() 
{
	if (!overview()) 
	{
		return -1;
	}

	if (!ccalllua())
	{
		return -1;
	}

	if (!luacallc())
	{
		return -1;
	}

	if (!metatable())
	{
		return -1;
	}

	if (!userdata()) 
	{
		return -1;
	}

	if (!coroutine())
	{
		return -1;
	}

	return 0;
}