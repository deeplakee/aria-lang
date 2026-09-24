-- sort_int 参照端口(与 sort_int.aria 同算法同规模)。
local n = 2500000

local t0 = os.clock()
local xs = {}
local state = 1
for i = 1, n do
    state = (state * 48271) % 2147483647
    xs[i] = state
end
table.sort(xs)

local checksum = 0
for i = 1, n do
    checksum = (checksum * 31 + xs[i] % 1009) % 1000000007
end
local dt = (os.clock() - t0) * 1000.0

assert(checksum == 947379667, "checksum drift")
assert(xs[1] == 145, "min drift")
assert(xs[n] == 2147483426, "max drift")
print("value checksum: " .. checksum)
print("value min: " .. xs[1])
print("value max: " .. xs[n])
print("bench-time: " .. dt .. " ms")
