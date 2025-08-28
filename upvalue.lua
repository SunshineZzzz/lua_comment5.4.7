local a = 1
local b = 2

function xxx()
	local c = 3
    print(a)

    function yyy()
    	print(a+b+c)
    end
end

xxx()
yyy()