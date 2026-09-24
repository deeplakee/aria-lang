-- word_count 参照端口(与 word_count.aria 同算法同规模)。
local function split(s, sep)
    local out, start = {}, 1
    while true do
        local i = s:find(sep, start, true)
        if not i then
            out[#out + 1] = s:sub(start)
            break
        end
        out[#out + 1] = s:sub(start, i - 1)
        start = i + #sep
    end
    return out
end

local vocab = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"}
local words = 3000000

local t0 = os.clock()
local state = 1
local parts = {}
for _ = 1, words do
    state = (state * 48271) % 2147483647
    parts[#parts + 1] = vocab[state % 8 + 1]
end
local text = table.concat(parts, " ")

local counts = {}
for _, w in ipairs(split(text, " ")) do
    counts[w] = (counts[w] or 0) + 1
end

local total = 0
for _, w in ipairs(vocab) do
    total = total + counts[w]
end
local dt = (os.clock() - t0) * 1000.0

assert(total == 3000000, "checksum drift")
assert(counts["alpha"] == 374756, "alpha drift")
assert(counts["theta"] == 374622, "theta drift")
print("value checksum: " .. total)
print("value alpha: " .. counts["alpha"])
print("value theta: " .. counts["theta"])
print("bench-time: " .. dt .. " ms")
