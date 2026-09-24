-- string_concat_compare 参照端口(与 string_concat_compare.aria 同算法同规模)。
local rounds = 2000000

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    local s = "abc" .. "def" .. "ghi"
    if s == "abcdefghi" then
        acc = acc + 1
    end
    if s < "abz" then
        acc = acc + 2
    end
    if s >= "abcdefgh" then
        acc = acc + 3
    end
    acc = (acc + #(s .. "!")) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999907, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
