-- list_slice 参照端口(与 list_slice.aria 同算法同规模):Lua 无切片,按本语言写法逐元素搬
-- 两个辅助函数都按 aria 的零基闭区间语义收参。
local function slice_asc(t, from, to)
    local out = {}
    for i = from, to do
        out[#out + 1] = t[i + 1]
    end
    return out
end

local function slice_desc(t, from, to)
    local out = {}
    for i = from, to, -1 do
        out[#out + 1] = t[i + 1]
    end
    return out
end

local size = 512
local rounds = 900000

local xs = {}
for i = 0, size - 1 do
    xs[i + 1] = i
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    local a = slice_asc(xs, 10, 20)
    local b = slice_asc(xs, size - 20, size - 1)
    local c = slice_desc(xs, 20, 10)
    acc = (acc + #a + #b + #c + a[1] + b[1] + c[1]) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 598479, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
