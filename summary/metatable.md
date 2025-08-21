1. getmetatable的c实现
```C
// getmetatable (object)
// If object does not have a metatable, returns nil.
// Otherwise, if the object's metatable has a "__metatable" field, 
// returns the associated value. 
// Otherwise, returns the metatable of the given object.
// 
// 假如我们想保护我们的对象使其使用者既看不到也不能修改metatables。
// 我们可以对metatable设置了__metatable的值，
// getmetatable将返回这个域的值，而调用setmetatable将会出错
static int luaB_getmetatable (lua_State *L) {
  luaL_checkany(L, 1);
  if (!lua_getmetatable(L, 1)) {
    lua_pushnil(L);
    return 1;  /* no metatable */
  }
  // 如果元表中已经有了__metatable则直接返回对应对象，如果没有返回元表本身即可
  luaL_getmetafield(L, 1, "__metatable");
  return 1;  /* returns either __metatable field (if present) or metatable */
}
```
2. setmetatable的c实现
```C
// setmetatable (table, metatable)
// Sets the metatable for the given table. 
// (You cannot change the metatable of other types from Lua, only from C.) 
// If metatable is nil, removes the metatable of the given table.
// If the original metatable has a "__metatable" field, raises an error.
// This function returns table.
// 
// 假如我们想保护我们的对象使其使用者既看不到也不能修改metatables。
// 我们可以对metatable设置了__metatable的值，getmetatable将返回这个域的值，
// 而调用setmetatable将会出错
static int luaB_setmetatable (lua_State *L) {
  // 传入的元表
  int t = lua_type(L, 2);
  // 要设置元表的目标表
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_argexpected(L, t == LUA_TNIL || t == LUA_TTABLE, 2, "nil or table");
  // 元表设置了__metatable，就会报错
  if (l_unlikely(luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL))
    return luaL_error(L, "cannot change a protected metatable");
  lua_settop(L, 2);
  // 设置元表
  lua_setmetatable(L, 1);
  return 1;
}
```