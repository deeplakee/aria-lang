-- exception_deep_unwind 参照端口(与 exception_deep_unwind.aria 同算法同规模)。
local function level3(v)
    error(v)
end

local function level2(v)
    level3(v)
    return -1
end

local function level1(v)
    level2(v)
    return -1
end

local iterations = 2600000

local t0 = os.clock()
local acc = 0
for i = 0, iterations - 1 do
    local ok, e = pcall(level1, i)
    if not ok then
        acc = (acc + e) % 1000003
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 560036, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
