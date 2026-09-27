-- markov 参照端口(与 markov.aria 同算法同规模)。
-- 词表与语料换到 1 基下标(vocab[rnd(n)+1]);状态表键为字符串、行是 [词, 计数, ...] 交织表
-- (词在奇数位、计数在偶数位);Lua 无 # 语义安全的字符串键计数,表大小用 pairs 计数。
local sentences = 6500
local gen_words = 20000

local vocab = {"the", "of", "and", "a", "to", "in", "is", "it", "was", "that", "for", "time",
               "world", "life", "mind", "light", "river", "stone", "forest", "winter", "summer",
               "harbor", "signal", "memory", "shadow", "window", "garden", "mirror", "voyage", "silence"}

local state = 12345
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local t0 = os.clock()

local corpus = {}
local corpus_n = 0
for _ = 1, sentences do
    local len = 6 + rnd(9)
    for _ = 1, len do
        corpus_n = corpus_n + 1
        corpus[corpus_n] = vocab[rnd(#vocab) + 1]
    end
end

local states = {}
local w0 = corpus[1]
local w1 = corpus[2]
for i = 3, corpus_n do
    local key = w0 .. " " .. w1
    local row = states[key]
    if row == nil then
        row = {}
        states[key] = row
    end
    local w2 = corpus[i]
    local found = false
    local p = 1
    while p <= #row do
        if row[p] == w2 then
            row[p + 1] = row[p + 1] + 1
            found = true
        end
        p = p + 2
    end
    if found == false then
        row[#row + 1] = w2
        row[#row + 1] = 1
    end
    w0 = w1
    w1 = w2
end

local produced = {}
local produced_n = 0
local checksum = 0
local g0 = corpus[1]
local g1 = corpus[2]
local made = 0
while made < gen_words do
    local row = states[g0 .. " " .. g1]
    if row == nil then
        g0 = corpus[1]
        g1 = corpus[2]
    else
        local total = 0
        local p = 2
        while p <= #row do
            total = total + row[p]
            p = p + 2
        end
        local pick = rnd(total)
        local w = row[1]
        p = 2
        while p <= #row do
            if pick < row[p] then
                w = row[p - 1]
                break
            end
            pick = pick - row[p]
            p = p + 2
        end
        produced_n = produced_n + 1
        produced[produced_n] = w
        checksum = (checksum * 31 + #w) % 1000003
        g0 = g1
        g1 = w
        made = made + 1
    end
end

local seen = {}
for i = 1, produced_n do
    seen[produced[i]] = 1
end
local distinct = 0
for _ in pairs(seen) do
    distinct = distinct + 1
end
local states_n = 0
for _ in pairs(states) do
    states_n = states_n + 1
end

assert(states_n == 900, "states drift")
assert(checksum == 679528, "checksum drift")
assert(distinct == 30, "distinct drift")

local dt = (os.clock() - t0) * 1000.0

print("value states: " .. states_n)
print("value checksum: " .. checksum)
print("value distinct: " .. distinct)
print("bench-time: " .. dt .. " ms")
