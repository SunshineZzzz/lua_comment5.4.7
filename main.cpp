#include<iostream>
#include "./src/lua.hpp"

// 脚本运行基础总览
bool overview()
{
	lua_State* lua = luaL_newstate();
	luaL_openlibs(lua);

	if (luaL_dofile(lua, "overview.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(lua, -1));
		lua_close(lua);
		return false;
	}

	lua_close(lua);
	return true;
}

// c调用lua
bool ccalllua()
{
	lua_State* lua = luaL_newstate();
	luaL_openlibs(lua);

	if (luaL_dofile(lua, "ccalllua.lua") != LUA_OK) 
	{
		fprintf(stderr, "Error: %s\n", lua_tostring(lua, -1));
		lua_close(lua);
		return false;
	}

	if (lua_getfield(lua, 1, "Add") != LUA_TFUNCTION)
	{
		fprintf(stderr, "get M.Add lua closure error\n");
		lua_close(lua);
		return false;
	}
	lua_pushinteger(lua, 1);
	lua_pushinteger(lua, 2);
	// 2个参数，1个返回值
	lua_call(lua, 2, 1);
	auto sum = lua_tointeger(lua, -1);
	fprintf(stdout, "M.Add(%d,%d) = %d\n", 1, 2, (int)sum);
	lua_pop(lua, 1);
	lua_pop(lua, 1);

	lua_getglobal(lua, "Add");
	lua_pushinteger(lua, 1);
	lua_pushinteger(lua, 2);
	lua_call(lua, 2, 1);
	sum = lua_tointeger(lua, -1);
	fprintf(stdout, "M.Add(%d,%d) = %d\n", 1, 2, (int)sum);
	lua_pop(lua, 1);

	lua_close(lua);
	return true;
}

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
	lua_State* lua = luaL_newstate();
	luaL_openlibs(lua);

	// 全局表压入栈
	lua_pushglobaltable(lua);
	// 把lua_cfunc_print中函数和名称压入全局表中
	lua_pushcfunction(lua, lua_cfunc_print);
	lua_setfield(lua, -2, "lua_cfunc_print");
	lua_pop(lua, 1);

	if (luaL_dofile(lua, "luacallc.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(lua, -1));
		lua_close(lua);
		return false;
	}

	lua_close(lua);
	return true;
}

bool metatable() 
{
	lua_State* lua = luaL_newstate();
	luaL_openlibs(lua);

	if (luaL_dofile(lua, "metatable.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(lua, -1));
		lua_close(lua);
		return false;
	}

	lua_close(lua);
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

	return 0;
}