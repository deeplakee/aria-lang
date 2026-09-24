-- class_field_rw 参照端口(与 class_field_rw.aria 同算法同规模):Lua 的对象就是表,字段读写即表槽。
local cell = {v = 0}
local iterations = 7000000

local t0 = os.clock()
local i = 0
while i < iterations do
    cell.v = cell.v + 1
    i = i + 1
end
local acc = cell.v
local dt = (os.clock() - t0) * 1000.0

assert(acc == 7000000, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
