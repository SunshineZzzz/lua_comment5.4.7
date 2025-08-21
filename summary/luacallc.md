```lua
lua_cfunc_print("hello", "world")
```

1. 暴露给lua的函数必须满足如下声明，int表示返回值个数
```c
typedef int (*lua_CFunction)(lua_State* L)
```

2. 注册到全局表中_G["lua_cfunc_print"] = lightcfunction
```c
// 全局表压入栈
lua_pushglobaltable(lua);
// 把lua_cfunc_print中函数和名称压入全局表中
lua_pushcfunction(lua, lua_cfunc_print);
lua_setfield(lua, -2, "lua_cfunc_print");
lua_pop(lua, 1);
```

3. 编译运行，调用到注册好的lightcfunction