-- 创建协程，没有运行
local co = coroutine.create(
    function(arg)
        print("start yield")
        local result = coroutine.yield(arg)
        print("restart co",  result)
        return result * arg
    end
)

local bRet, val = coroutine.resume(co, 1)
print(bRet, val)

bRet, val = coroutine.resume(co, 2)
print(bRet, val)

bRet, val  = coroutine.resume(co, 3)
print(bRet, val)