-- destructure_assign 参照端口(与 destructure_assign.aria 同算法同规模):多赋值即 Lua 的解构;
-- rest 按本语言写法取「首个之后的元素个数」。
local rounds = 1200000

local p, q = 1, 2

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    p, q = q, p + q
    p = p % 1000003
    q = q % 1000003
    local t = {p, q, p + q}
    local x = t[1]
    local rest_count = #t - 1
    acc = (acc + x + rest_count) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 180840, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
