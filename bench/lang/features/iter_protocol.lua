-- iter_protocol 参照端口(与 iter_protocol.aria 同算法同规模):四种源按本语言的迭代写法遍历。
local size = 512
local rounds = 4000

local xs = {}
for i = 0, size - 1 do
    xs[i + 1] = i
end
local m = {}
for k = 0, size - 1 do
    m[k] = k
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    for _, v in ipairs(xs) do
        acc = (acc + v) % 1000003
    end
    for v = 0, size - 1 do
        acc = (acc + v) % 1000003
    end
    for _ in ("abc"):gmatch(".") do
        acc = acc + 1
    end
    for k in pairs(m) do
        acc = (acc + k) % 1000003
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 799293, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
