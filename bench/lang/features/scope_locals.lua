-- scope_locals 参照端口(与 scope_locals.aria 同算法同规模)。
local function work(a)
    local b = a + 1
    local c = b * 2
    local d = c - 3
    local out = d
    do
        local e = d + 4
        out = e % 1000003
    end
    return out
end

local rounds = 4500000

local t0 = os.clock()
local acc = 0
local i = 0
while i < rounds do
    acc = (acc + work(i)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 250156, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
