-- recursion_fib 参照端口(与 recursion_fib.aria 同算法同规模)。
local function fib(n)
    if n < 2 then
        return n
    end
    return fib(n - 1) + fib(n - 2)
end

local rounds = 20
local fib_input = 27

local t0 = os.clock()
local acc = 0
local r = 0
while r < rounds do
    acc = acc + fib(fib_input)
    r = r + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 3928360, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
