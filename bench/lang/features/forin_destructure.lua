-- forin_destructure 参照端口(与 forin_destructure.aria 同算法同规模):pairs 逐对遍历。
local size = 256
local rounds = 13000

local m = {}
for i = 0, size - 1 do
    m[i] = i * 2
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    for k, v in pairs(m) do
        acc = (acc + k + v) % 1000003
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 956184, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
