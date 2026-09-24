-- string_codepoint 参照端口(与 string_codepoint.aria 同算法同规模):码点域用 utf8 库;字节域口径
-- 各语言不同,不进校验和。
local rounds = 1100000
local base = "héllo wörld"

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    -- aria 的 codepoint_at(i) 按码点下标取,utf8.codepoint 收字节位置:此处第 2 个码点落在字节 2、
-- 第 5 个码点(o)落在字节 6,故分别是 2 与 6。
    acc = (acc + utf8.len(base) + utf8.codepoint(base, 2) + utf8.codepoint(base, 6) +
           utf8.codepoint(base, 1)) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 898488, "checksum drift")
assert(utf8.len(base) == 11, "codepoints drift")
print("value checksum: " .. acc)
print("value codepoints: " .. utf8.len(base))
print("bench-time: " .. dt .. " ms")
