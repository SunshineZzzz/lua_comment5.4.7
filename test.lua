local m = require("mod")
local f = m.f
local a = m.a

print(a)
f()

--[[
local function test1()
    return 1, 2
end
local a, b = 1, 2
a, b = test1(), test1()
a, b = test1()

local function test2()
	local t = {}
	local function inner()
		t[1], t = 1, 1
	end
	inner()
end
test2()

if not "hello" then
end

local x <const> = 100000
if x == 100000 then
	local t = {1, 2, 3, a = 1, ["b"] = 2}
	local k = "a"
	local x = t[k]
	local y = 1
	t[y], y = y, 3
elseif x >= 2 then
else
end

flag = false
do
	local x <close> = setmetatable({}, {__close = function () flag = true end})
	local x = {}
end
print(flag)

co = coroutine.create(function()
	for k, v in pairs({"a", "b", "c"}) do
		coroutine.yield(k, v)
	end
	return "d", 4
end)
print(coroutine.resume(co)) --> true  1  a
print(coroutine.resume(co)) --> true  2  b
print(coroutine.resume(co)) --> true  3  c
print(coroutine.resume(co)) --> true  d  4
print(coroutine.resume(co)) --> false cannot resume dead coroutin
]]