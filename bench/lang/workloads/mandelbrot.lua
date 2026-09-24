-- mandelbrot 参照端口(与 mandelbrot.aria 同算法同规模)。
local size = 500
local max_iter = 50

local t0 = os.clock()
local checksum = 0
local inside = 0
for py = 0, size - 1 do
    local y0 = (py * 1.0 / size) * 2.0 - 1.0
    for px = 0, size - 1 do
        local x0 = (px * 1.0 / size) * 2.5 - 2.0
        local x, y = 0.0, 0.0
        local i = 0
        while i < max_iter do
            local x2, y2 = x * x, y * y
            if x2 + y2 > 4.0 then
                break
            end
            y = 2.0 * x * y + y0
            x = x2 - y2 + x0
            i = i + 1
        end
        checksum = (checksum * 2 + i) % 1000000007
        if i == max_iter then
            inside = inside + 1
        end
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(checksum == 140394512, "checksum drift")
assert(inside == 79596, "inside drift")
assert(size * size == 250000, "pixels drift")
print("value checksum: " .. checksum)
print("value inside: " .. inside)
print("value pixels: " .. size * size)
print("bench-time: " .. dt .. " ms")
