-- brainfuck 参照端口(与 brainfuck.aria 同算法同规模):预处理 + 逐程序自校验在计时段外,
-- t0 之后只有 rounds 轮 x 24 个已编译程序的重复执行段。位置一律 1 起(code/pc/jumps/tape/dp
-- 按 Lua 惯例 1 基),配对跳转协议与 aria 版一致。
local suite_n = 24   -- 规模:生成程序数
local rounds = 70    -- 规模:执行轮数

local state = 91
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local function preprocess(src)
    local code, jumps, stack = {}, {}, {}
    for i = 1, #src do
        local c = src:sub(i, i)
        if c == "+" or c == "-" or c == "<" or c == ">" or c == "." or c == "," or c == "[" or c == "]" then
            if c == "[" then
                stack[#stack + 1] = #code + 1
            elseif c == "]" then
                if #stack == 0 then
                    error("bf: unmatched ]")
                end
                local open = table.remove(stack)
                jumps[open] = #code + 1
                jumps[#code + 1] = open
            end
            code[#code + 1] = c
        end
    end
    if #stack ~= 0 then
        error("bf: unmatched [")
    end
    return code, jumps
end

local function run_bf(code, jumps)
    local tape = {}
    for t = 1, 64 do
        tape[t] = 0
    end
    local out = 0
    local dp = 1
    local pc = 1
    local n = #code
    while pc <= n do
        local c = code[pc]
        if c == "-" then
            tape[dp] = tape[dp] - 1
        elseif c == "+" then
            tape[dp] = tape[dp] + 1
        elseif c == "<" then
            dp = dp - 1
        elseif c == ">" then
            dp = dp + 1
        elseif c == "[" then
            if tape[dp] == 0 then
                pc = jumps[pc]
            end
        elseif c == "]" then
            -- 回跳后让公共的 pc = pc + 1 恰好落在配对 [ 上重新判零
            pc = jumps[pc] - 1
        elseif c == "." then
            out = out + tape[dp]
        end
        pc = pc + 1
    end
    return out
end

-- 乘法打印程序:cell0 = a,cell1 累加出 a*b 后逐次递减打印([.-] 输出 a*b..1 的码点)。
local function make_mul_prog(a, b)
    local parts = {}
    for _ = 1, a do
        parts[#parts + 1] = "+"
    end
    parts[#parts + 1] = "[>"
    for _ = 1, b do
        parts[#parts + 1] = "+"
    end
    parts[#parts + 1] = "<-]>"
    parts[#parts + 1] = "[.-]"
    return table.concat(parts)
end

-- 期望码点和 = a*b*(a*b+1)/2,封闭式自校验(计时段外)。
local sources, expected = {}, {}
for _ = 1, suite_n do
    local a = 4 + rnd(16)
    local b = 4 + rnd(16)
    sources[#sources + 1] = make_mul_prog(a, b)
    expected[#expected + 1] = a * b * (a * b + 1) // 2
end

local compiled = {}
for i = 1, #sources do
    local code, jumps = preprocess(sources[i])
    if run_bf(code, jumps) ~= expected[i] then
        error("bf: suite self-check failed")
    end
    compiled[#compiled + 1] = {code, jumps}
end

local t0 = os.clock()
local total = 0
for _ = 1, rounds do
    for _, cm in ipairs(compiled) do
        total = (total + run_bf(cm[1], cm[2])) % 1000003
    end
end
assert(total == 632355, "brainfuck checksum drift")
local dt = (os.clock() - t0) * 1000.0

print("value checksum: " .. total)
print("bench-time: " .. dt .. " ms")
