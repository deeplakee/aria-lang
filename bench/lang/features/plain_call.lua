-- plain_call 参照端口(与 plain_call.aria 同算法同规模)。
local function step(v)
    return v + 1
end

local iterations = 6000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = acc + step(i)
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 18000003000000, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
