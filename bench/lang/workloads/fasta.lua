-- fasta 参照端口(与 fasta.aria 同算法同规模):不含经典版的行宽折行。
local im = 139968
local ia = 3877
local ic = 29573
local state = 42

local chars = {"a", "c", "g", "t", "B", "D", "H", "K", "M", "N", "R", "S", "V", "W", "Y"}
local probs = {0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02}
local cum = {}
local running = 0.0
for i = 1, #probs do
    running = running + probs[i]
    cum[i] = running
end

local bases = 2000000

local t0 = os.clock()
local parts = {}
local checksum = 0
for _ = 1, bases do
    state = (state * ia + ic) % im
    local pick = state * 1.0 / im
    local i = 1
    while i <= 15 and cum[i] <= pick do
        i = i + 1
    end
    if i == 16 then
        i = 15
    end
    parts[#parts + 1] = chars[i]
    checksum = (checksum * 131 + string.byte(chars[i])) % 1000000007
end
local seq = table.concat(parts, "")
local dt = (os.clock() - t0) * 1000.0

assert(checksum == 575313243, "checksum drift")
assert(#seq == 2000000, "length drift")
print("value checksum: " .. checksum)
print("value length: " .. #seq)
print("bench-time: " .. dt .. " ms")
