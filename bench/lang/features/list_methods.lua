-- list_methods 参照端口(与 list_methods.aria 同算法同规模):find/contains 是手写扫描(本语言的
-- 自然写法),表长恒定故 size 用 #t;find 的 1-based 下标减 1 对齐零基读数。
local size = 2000
local rounds = 800000

local xs = {}
for i = 0, size - 1 do
    xs[i + 1] = i
end

local t0 = os.clock()
local acc = 0
for r = 0, rounds - 1 do
    table.insert(xs, r)
    table.insert(xs, 1, r)
    local idx
    for k = 1, #xs do
        if xs[k] == r then
            idx = k - 1
            break
        end
    end
    if idx ~= nil then
        acc = (acc + idx) % 1000003
    end
    acc = (acc + table.remove(xs, 3)) % 1000003
    acc = (acc + table.remove(xs)) % 1000003
    acc = (acc + #xs) % 1000003
end
local joined = table.concat(xs, ",")
acc = (acc + #joined) % 1000003
local dt = (os.clock() - t0) * 1000.0

assert(acc == 684118, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
