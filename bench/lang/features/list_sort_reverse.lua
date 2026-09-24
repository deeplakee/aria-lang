-- list_sort_reverse 参照端口(与 list_sort_reverse.aria 同算法同规模):反转是手写就地交换
-- (Lua 无 reverse 原语)。
local size = 200000
local rounds = 20

local xs = {}
local state = 1
for i = 1, size do
    state = (state * 48271) % 2147483647
    xs[i] = state
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    table.sort(xs)
    local lo, hi = 1, size
    while lo < hi do
        local tmp = xs[lo]
        xs[lo] = xs[hi]
        xs[hi] = tmp
        lo = lo + 1
        hi = hi - 1
    end
    for j = 1, size do
        acc = (acc * 31 + xs[j] % 1009) % 1000000007
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 745687805, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
