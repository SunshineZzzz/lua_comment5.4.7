#include <iostream>
#include "./src/lua.hpp"

namespace n_overview {
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

		// lua_pcall(L, 0, LUA_MULTRET, 0)，调用一个(c/lua)函数期望的返回值，有多少返回值就返回多少返回值
		const char* pArg2 = luaL_checkstring(L, -1);
		const char* pArg1 = luaL_checkstring(L, -2);

        fprintf(stdout, "overview.lua = %s,%s\n", pArg1, pArg2);

		lua_close(L);
		return true;
	}
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_ccalllua {
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
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_luacallc {
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
    static int lua_closurefunc_print(lua_State* L)
    { 
		auto up1 = lua_tostring(L, lua_upvalueindex(1));
		auto up2 = lua_tostring(L, lua_upvalueindex(1));
        lua_writestring(up1, strlen(up1));
		lua_writestring(up1, strlen(up1));
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

		lua_pushglobaltable(L);
		lua_pushstring(L, "cclosure1");
		lua_pushstring(L, "cclosure2");
		lua_pushcclosure(L, lua_closurefunc_print, 2);
        lua_setfield(L, -2, "lua_closurefunc_print");
        lua_pop(L, 1);

		if (luaL_dofile(L, "luacallc.lua") != LUA_OK) {
			fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
			lua_close(L);
			return false;
		}

		lua_close(L);
		return true;
	}
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_metatable {
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
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_userdata {
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
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_lightuserdata
{
	// lightuserdata
	struct people
	{
		char name[128];
		int age;

		people()
		{
			memset(name, 0, 128);
			age = 0;
		}
	};
	static int GetAttribute(lua_State* L)
	{
		auto* pData = (people*)lua_touserdata(L, 1);
		std::string attribute = luaL_checkstring(L, 2);
		if (attribute == "name")
		{
			lua_pushstring(L, pData->name);
			return 1;
		}
		else if (attribute == "age")
		{
			lua_pushinteger(L, pData->age);
			return 1;
		}
		else
		{
			lua_pushstring(L, "invalid attribute");
			return 1;
		}
	}
	static int SetAttribute(lua_State* L)
	{
		auto* pData = (people*)lua_touserdata(L, 1);
        std::string attribute = luaL_checkstring(L, 2);
        if (attribute == "name")
        {
            const char* pNewName = luaL_checkstring(L, 3);
            if (pNewName == NULL)
            {
                return 0;
            }
            strncpy_s(pData->name, pNewName, 128);
            return 0;
        }
        else if (attribute == "age")
        {
            pData->age = luaL_checkinteger(L, 3);
            return 0;
        }
        else
        {
            lua_pushstring(L, "invalid attribute");
            return 1;
        }
	}
	static  luaL_Reg people_mts[] = 
	{
		{ "__index", GetAttribute },
		{ "__newindex", SetAttribute },
		{ NULL, NULL }
	};
	bool lightuserdata()
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

		// 创建lightuserdata，并且设置元表
		auto pPeople = std::make_unique<people>();
		lua_pushlightuserdata(L, pPeople.get());
		luaL_getmetatable(L, "peopleMT");
		lua_setmetatable(L, -2);
		// 将lightuserdata保存到全局表中
		lua_setglobal(L, "gPeople");
		

	    if (luaL_dofile(L, "lightuserdata.lua") != LUA_OK)
		{
			fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
			lua_close(L);
			return false;
		}

		lua_close(L);
		return true;
	}
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_coroutine {
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
}

/////////////////////////////////////////////////////////////////////////////////

namespace n_upvalue 
{
	bool upvalue()
	{
		lua_State* L = luaL_newstate();
		luaL_openlibs(L);

		if (luaL_dofile(L, "upvalue.lua") != LUA_OK)
		{
			fprintf(stderr, "Error: %s\n", lua_tostring(L, -1));
			lua_close(L);
			return false;
		}

		lua_close(L);
		return true;
	}
}

int main() 
{
	if (!n_overview::overview())
	{
		return -1;
	}

	if (!n_ccalllua::ccalllua())
	{
		return -1;
	}

	if (!n_luacallc::luacallc())
	{
		return -1;
	}

	if (!n_metatable::metatable())
	{
		return -1;
	}

	if (!n_userdata::userdata()) 
	{
		return -1;
	}

	if (!n_lightuserdata::lightuserdata())
	{
		return -1;
	}

	if (!n_coroutine::coroutine())
	{
		return -1;
	}

	if (!n_upvalue::upvalue())
	{
		return -1;
	}

	return 0;
}