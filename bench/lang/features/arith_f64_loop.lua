-- arith_f64_loop 参照端口(与 arith_f64_loop.aria 同算法同规模)。
local iterations = 5000000

local t0 = os.clock()
local x = 0.5
local hits = 0
local i = 0
while i < iterations do
    x = x * 1.0000001 + 0.25
    if x > 100.0 then
        x = x / 2.0
    end
    if x > 50.0 then
        hits = hits + 1
    end
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(hits == 4999803, "checksum drift")
print("value checksum: " .. hits)
print("bench-time: " .. dt .. " ms")
