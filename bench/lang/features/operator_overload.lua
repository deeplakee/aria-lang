-- operator_overload 参照端口(与 operator_overload.aria 同算法同规模):元表 __add 即 Lua 的算子重载。
local Box = {}
Box.__index = Box

function Box.new(v)
    return setmetatable({v = v}, Box)
end

Box.__add = function(a, b)
    return a.v + b.v
end

local a = Box.new(3)
local b = Box.new(4)
local iterations = 4000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + (a + b)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999919, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
