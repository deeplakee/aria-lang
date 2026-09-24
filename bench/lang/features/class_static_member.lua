-- class_static_member 参照端口(与 class_static_member.aria 同算法同规模):表的字段即静态成员。
local Counter = {n = 0}

function Counter.add(d)
    Counter.n = Counter.n + d
    return Counter.n
end

local iterations = 4200000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + Counter.add(1)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 640075, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
