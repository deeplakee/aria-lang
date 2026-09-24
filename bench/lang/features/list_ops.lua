-- list_ops 参照端口(与 list_ops.aria 同算法同规模):Lua 表 1-based,下标换算成 0-based 读数。
local size = 200000
local rounds = 20

local xs = {}
for i = 0, size - 1 do
    xs[i + 1] = i
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    for j = 0, size - 1 do
        local a = xs[j + 1]
        local b = xs[size - j]
        acc = (acc + a + b) % 1000003
        xs[j + 1] = (a + 1) % 1000003
    end
end
acc = (acc + #xs) % 1000003
local dt = (os.clock() - t0) * 1000.0

assert(acc == 799787, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
