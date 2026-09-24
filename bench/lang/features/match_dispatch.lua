-- match_dispatch 参照端口(与 match_dispatch.aria 同算法同规模):Lua 无 match,用本语言的多路
-- 派发写法 if/elseif 链(近似:与 switch/字典派发机制不同)。
local iterations = 4000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    local key = i % 5
    local bucket
    if key == 0 then
        bucket = 3
    elseif key == 1 then
        bucket = 5
    elseif key == 2 then
        bucket = 7
    elseif key == 3 then
        bucket = 11
    else
        bucket = 13
    end
    acc = (acc + bucket) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 199907, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
