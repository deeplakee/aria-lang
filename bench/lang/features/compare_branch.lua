-- compare_branch 参照端口(与 compare_branch.aria 同算法同规模)。
local iterations = 4000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    if i % 15 == 0 then
        acc = acc + 3
    elseif i % 5 == 0 then
        acc = acc + 2
    elseif i % 3 == 0 then
        acc = acc + 1
    else
        acc = acc + 0
    end
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 2933334, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
