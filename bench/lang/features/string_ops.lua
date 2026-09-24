-- string_ops 参照端口(与 string_ops.aria 同算法同规模):Lua 无 split,手写(保留空段,与 aria 的
-- 单参 split 同);find 的 1-based 下标减 1 对齐零基读数。
local function split(s, sep)
    local out, start = {}, 1
    while true do
        local i = s:find(sep, start, true)
        if not i then
            out[#out + 1] = s:sub(start)
            break
        end
        out[#out + 1] = s:sub(start, i - 1)
        start = i + #sep
    end
    return out
end

local rounds = 1000000
local base = "the quick brown fox jumps over the lazy dog"

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    if base:find("brown", 1, true) then
        acc = acc + (base:find("fox", 1, true) - 1)
    end
    local parts = split(base, " ")
    acc = (acc + #parts + #(base:sub(5, 9))) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999913, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
