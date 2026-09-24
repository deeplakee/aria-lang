-- logic_short_circuit 参照端口(与 logic_short_circuit.aria 同算法同规模)。
local iterations = 6000000

local t0 = os.clock()
local acc = 0
local i = 0
while i < iterations do
    if (i > 0 and i % 2 == 0) or i % 7 == 0 then
        acc = acc + 1
    end
    i = i + 1
end
local dt = (os.clock() - t0) * 1000.0

assert(acc == 3428571, "checksum drift")
print("value checksum: " .. acc)
print("bench-time: " .. dt .. " ms")
