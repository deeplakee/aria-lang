-- closure_upvalue 参照端口(与 closure_upvalue.aria 同算法同规模)。
local function make_counter()
    local n = 0
    return function()
        n = n + 1
        return n
    end
end

local next_value = make_counter()
local iterations = 6000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = acc + next_value()
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 18000003000000, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
