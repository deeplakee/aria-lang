-- destructure_slice 参照端口(与 destructure_slice.aria 同算法同规模)。
local iterations = 2000000
local pair = {1, 2}
local xs = {10, 20, 30, 40, 50}

local t0 = os.clock()
local acc = 0
for _ = 1, iterations do
    local a, b = pair[1], pair[2]
    local x, y = xs[2], xs[3]
    acc = (acc + a + b + x + y) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999685, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
