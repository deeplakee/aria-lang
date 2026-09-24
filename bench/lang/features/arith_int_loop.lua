-- arith_int_loop 参照端口(与 arith_int_loop.aria 同算法同规模)。
local iterations = 5000000

local t0 = os.clock()
local acc = 1
local i = 0
while i < iterations do
    acc = (acc + i) % 1000003
    acc = acc * 3 - 1
    acc = acc + i % 7
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 109608, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
