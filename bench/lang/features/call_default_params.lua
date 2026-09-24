-- call_default_params 参照端口(与 call_default_params.aria 同算法同规模):Lua 无默认参数,用本语言
-- 的写法(形参为 nil 时补默认值);aria 是编译期垫充,机制不同、读数同量级。
local function step(v, k)
    if k == nil then
        k = 3
    end
    return v + k
end

local iterations = 5500000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + step(i)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 375096, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
