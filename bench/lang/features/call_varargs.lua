-- call_varargs 参照端口(与 call_varargs.aria 同算法同规模)。
local function total(...)
    local s = 0
    for _, v in ipairs({...}) do
        s = s + v
    end
    return s
end

local iterations = 1300000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + total(i, i + 1, i + 2)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 345018, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
