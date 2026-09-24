-- lambda_higher_order 参照端口(与 lambda_higher_order.aria 同算法同规模)。
local function compose(f, g)
    return function(x)
        return f(g(x))
    end
end

local inc = function(x) return x + 1 end
local dbl = function(x) return x * 2 end
local chain = compose(inc, dbl)
local iterations = 5000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    acc = chain(acc) % 1000003
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 395508, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
