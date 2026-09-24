-- call_hook 参照端口(与 call_hook.aria 同算法同规模):元表 __call 即 Lua 的调用钩子。
local function adder(k)
    return setmetatable({k = k}, {__call = function(self, x) return self.k + x end})
end

local add2 = adder(2)
local iterations = 6000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = (acc + add2(i)) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 135, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
