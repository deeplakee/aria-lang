-- sieve 参照端口(与 sieve.aria 同算法同规模):Lua 无字节数组,用表(与 aria 的 0/1 整数表同性质)。
local t0 = os.clock()
local total = 0
local counts = {}
for _, n in ipairs({100000, 1000000, 4000000}) do
    local composite = {}
    local count = 0
    local i = 2
    while i < n do
        if composite[i] == nil then
            count = count + 1
            local j = i * i
            while j < n do
                composite[j] = true
                j = j + i
            end
        end
        i = i + 1
    end
    total = total + count
    counts[#counts + 1] = count
    print("value count_" .. n .. ": " .. count)
end
local dt = (os.clock() - t0) * 1000.0

assert(counts[1] == 9592, "count_100000 drift")
assert(counts[2] == 78498, "count_1000000 drift")
assert(counts[3] == 283146, "count_4000000 drift")
assert(total == 371236, "checksum drift")
print("value checksum: " .. total)
print("bench-time: " .. dt .. " ms")
