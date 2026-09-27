-- huffman 参照端口(与 huffman.aria 同算法同规模)。
-- 1 基换算:letters/weights 下标 +1,字符经 sub 取单字符;建树队列按词首次出现序(不依赖表迭代序)。
local messages = 1150
local msg_len = 80

local letters = "etaoinshrdlucmfwypvbgkxjqz"
local weights = {113, 90, 80, 75, 70, 67, 63, 61, 60, 43, 40, 28, 24, 22, 21, 19, 19, 18, 17, 16, 15, 13, 10, 9, 5, 2}

local state = 31337
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local function gen_char()
    local r = rnd(1000)
    local acc = 0
    for i = 1, #weights do
        acc = acc + weights[i]
        if r < acc then
            return letters:sub(i, i)
        end
    end
    return letters:sub(1, 1)
end

local t0 = os.clock()

local texts = {}
for _ = 1, messages do
    local parts = {}
    for _ = 1, msg_len do
        parts[#parts + 1] = gen_char()
    end
    texts[#texts + 1] = table.concat(parts)
end

local freq = {}
local order = {}
for _, msg in ipairs(texts) do
    for i = 1, #msg do
        local ch = msg:sub(i, i)
        if freq[ch] ~= nil then
            freq[ch] = freq[ch] + 1
        else
            freq[ch] = 1
            order[#order + 1] = ch
        end
    end
end

local pq = {}
for _, ch in ipairs(order) do
    local n = freq[ch]
    local pos = #pq + 1
    while pos > 1 and pq[pos - 1].w > n do
        pos = pos - 1
    end
    table.insert(pq, pos, {w = n, ch = ch, left = nil, right = nil})
end
while #pq > 1 do
    local a = table.remove(pq, 1)
    local b = table.remove(pq, 1)
    local merged = {w = a.w + b.w, ch = "", left = a, right = b}
    local pos = #pq + 1
    while pos > 1 and pq[pos - 1].w > merged.w do
        pos = pos - 1
    end
    table.insert(pq, pos, merged)
end
local root = pq[1]

local codes = {}
local function walk(node, prefix)
    if node.left == nil then
        codes[node.ch] = prefix
        return
    end
    walk(node.left, prefix .. "0")
    walk(node.right, prefix .. "1")
end
walk(root, "")

local bit_total = 0
local encoded = {}
for _, msg in ipairs(texts) do
    local bits = {}
    for i = 1, #msg do
        local ch = msg:sub(i, i)
        local code = codes[ch]
        bit_total = bit_total + #code
        for j = 1, #code do
            bits[#bits + 1] = code:sub(j, j)
        end
    end
    encoded[#encoded + 1] = table.concat(bits)
end

local decoded_ok = 0
for di = 1, messages do
    local enc = encoded[di]
    local node = root
    local out = {}
    for i = 1, #enc do
        if enc:sub(i, i) == "0" then
            node = node.left
        else
            node = node.right
        end
        if node.left == nil then
            out[#out + 1] = node.ch
            node = root
        end
    end
    if table.concat(out) == texts[di] then
        decoded_ok = decoded_ok + 1
    end
end

local codes_n = 0
for _ in pairs(codes) do
    codes_n = codes_n + 1
end

assert(codes_n == 26, "symbols drift")
assert(bit_total == 395969, "bit_total drift")
assert(decoded_ok == messages, "decode drift")

local dt = (os.clock() - t0) * 1000.0

print("value symbols: " .. codes_n)
print("value bit_total: " .. bit_total)
print("value decoded_ok: " .. decoded_ok)
print("bench-time: " .. dt .. " ms")
