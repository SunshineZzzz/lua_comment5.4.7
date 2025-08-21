local t = {}
setmetatable(t, {__metatable = "You cannot access here"})
print(getmetatable(t))