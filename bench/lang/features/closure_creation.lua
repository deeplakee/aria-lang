-- closure_creation 参照端口(与 closure_creation.aria 同算法同规模)。
local function make(base)
    return function(d)
        return base + d
    end
end

local iterations = 2800000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    local f = make(i)
    acc = (acc + f(2)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 440024, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
