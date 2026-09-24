-- exception_hot 参照端口(与 exception_hot.aria 同算法同规模):pcall + error(数值载荷原值透传)。
local iterations = 8000000

local t0 = os.clock()
local acc = 0
for i = 0, iterations - 1 do
    local ok, e = pcall(function() error(i) end)
    if not ok then
        acc = (acc + e) % 1000003
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 300, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
