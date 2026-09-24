-- matmul_int 参照端口(与 matmul_int.aria 同算法同规模)。
local n = 250

local t0 = os.clock()
local a, b = {}, {}
for i = 0, n - 1 do
    local row = {}
    for j = 0, n - 1 do
        row[j + 1] = (i * 7 + j * 13 + 1) % 1009
    end
    a[i + 1] = row
end
for i = 0, n - 1 do
    local row = {}
    for j = 0, n - 1 do
        row[j + 1] = (i * 11 + j * 3 + 2) % 1009
    end
    b[i + 1] = row
end

local checksum = 0
for i = 0, n - 1 do
    local arow = a[i + 1]
    for j = 0, n - 1 do
        local acc = 0
        for k = 0, n - 1 do
            acc = acc + arow[k + 1] * b[k + 1][j + 1]
        end
        checksum = (checksum * 31 + acc) % 1000000007
    end
end
local dt = (os.clock() - t0) * 1000.0

assert(checksum == 396310743, "checksum drift")
print("value checksum: " .. checksum)
print("bench-time: " .. dt .. " ms")
