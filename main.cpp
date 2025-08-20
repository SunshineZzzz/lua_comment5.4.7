#include<iostream>
#include "./src/lua.hpp"

int main() {
	lua_State* lua = luaL_newstate();
	luaL_openlibs(lua);

	if (luaL_dofile(lua, "overview.lua") != LUA_OK) {
		fprintf(stderr, "Error: %s\n", lua_tostring(lua, -1));
		lua_close(lua);
		return 1;
	}
	lua_close(lua);
	return 0;
}