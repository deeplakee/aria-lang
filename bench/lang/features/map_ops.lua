-- map_ops 参照端口(与 map_ops.aria 同算法同规模):键是整数值 0..entries-1(与 aria 同键集);
-- Lua 无 O(1) 表长,而本表建好后不改,故 size 直接用常量 entries。
local entries = 100000
local rounds = 40

local m = {}
for i = 0, entries - 1 do
    m[i] = i * 2
end

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    for j = 0, entries - 1 do
        local v = m[j]
        if v ~= nil then
            acc = (acc + v) % 1000003
        end
    end
end
acc = (acc + entries) % 1000003
local dt = (os.clock() - t0) * 1000.0

assert(acc == 900018, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
