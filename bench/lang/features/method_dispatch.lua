-- method_dispatch 参照端口(与 method_dispatch.aria 同算法同规模):元表 __index 提供方法面。
local Counter = {}
Counter.__index = Counter

function Counter.new()
    return setmetatable({base = 1}, Counter)
end

function Counter:step(delta)
    return self.base + delta
end

local counter = Counter.new()
local iterations = 5000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + counter:step(i)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 105, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
