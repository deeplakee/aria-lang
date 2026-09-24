-- loop_forms 参照端口(与 loop_forms.aria 同算法同规模):Lua 无 continue,用本语言的 goto 标签写法。
local rounds = 500000

local t0 = os.clock()
local acc = 0
for _ = 1, rounds do
    for k = 0, 15 do
        if k % 3 == 0 then
            goto continue
        end
        if k > 12 then
            break
        end
        acc = (acc + k) % 1000003
        ::continue::
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 999931, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
