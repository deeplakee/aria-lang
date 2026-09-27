-- word_diff 参照端口(与 word_diff.aria 同算法同规模):LCG 造基准文档,按比例做替换/插词/删词得
-- 修订版,再对每对版本跑 LCS 全表动态规划 + 回溯,统计保留/删除/插入词数(评审工具的词级 diff)。
local doc_size = 130   -- 规模:文档词数
local pair_count = 42  -- 规模:版本对数

local vocab = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota",
               "kappa", "lambda", "mu"}

local state = 555
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local function gen_doc()
    local d = {}
    for _ = 1, doc_size do
        d[#d + 1] = vocab[rnd(#vocab) + 1]
    end
    return d
end

local function mutate(src)
    local out = {}
    for i = 1, #src do
        local roll = rnd(20)
        if roll == 0 then
            -- 删词:跳过
        elseif roll == 1 then
            out[#out + 1] = vocab[rnd(#vocab) + 1]
        elseif roll == 2 then
            out[#out + 1] = src[i]
            out[#out + 1] = vocab[rnd(#vocab) + 1]
        else
            out[#out + 1] = src[i]
        end
    end
    return out
end

-- DP 表换算成 1 起下标:lua t[i+1][j+1] 存 aria 的 t[i][j],词表 lua a[i] 即 aria 的 a[i-1]。
local function diff_counts(a, b)
    local n1 = #a
    local n2 = #b
    local t = {}
    for i = 0, n1 do
        local row = {}
        for j = 0, n2 do
            row[j + 1] = 0
        end
        t[i + 1] = row
    end
    for i = 1, n1 do
        for j = 1, n2 do
            if a[i] == b[j] then
                t[i + 1][j + 1] = t[i][j] + 1
            else
                local up = t[i][j + 1]
                local left = t[i + 1][j]
                if up >= left then
                    t[i + 1][j + 1] = up
                else
                    t[i + 1][j + 1] = left
                end
            end
        end
    end
    local eq = 0
    local del = 0
    local ins = 0
    local i = n1
    local j = n2
    while i > 0 and j > 0 do
        if a[i] == b[j] then
            eq = eq + 1
            i = i - 1
            j = j - 1
        elseif t[i][j + 1] >= t[i + 1][j] then
            del = del + 1
            i = i - 1
        else
            ins = ins + 1
            j = j - 1
        end
    end
    del = del + i
    ins = ins + j
    return eq, del, ins
end

local t0 = os.clock()
local eq_sum = 0
local del_sum = 0
local ins_sum = 0
for _ = 1, pair_count do
    local a = gen_doc()
    local b = mutate(a)
    local eq, del, ins = diff_counts(a, b)
    eq_sum = (eq_sum + eq) % 1000003
    del_sum = (del_sum + del) % 1000003
    ins_sum = (ins_sum + ins) % 1000003
end
local dt = (os.clock() - t0) * 1000.0

assert(eq_sum == 4966, "eq_sum drift")
assert(del_sum == 494, "del_sum drift")
assert(ins_sum == 503, "ins_sum drift")
print("value eq_sum: " .. eq_sum)
print("value del_sum: " .. del_sum)
print("value ins_sum: " .. ins_sum)
print("bench-time: " .. dt .. " ms")
