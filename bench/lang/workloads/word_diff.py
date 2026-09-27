"""word_diff 参照端口(与 word_diff.aria 同算法同规模):LCG 造基准文档,按比例做替换/插词/删词得
修订版,再对每对版本跑 LCS 全表动态规划 + 回溯,统计保留/删除/插入词数(评审工具的词级 diff)。"""
import time

doc_size = 130  # 规模:文档词数
pairs = 42      # 规模:版本对数

vocab = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa",
         "lambda", "mu"]

state = 555


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


def gen_doc():
    d = []
    i = 0
    while i < doc_size:
        d.append(vocab[rnd(len(vocab))])
        i += 1
    return d


def mutate(src):
    out = []
    i = 0
    while i < len(src):
        roll = rnd(20)
        if roll == 0:
            pass  # 删词:跳过
        elif roll == 1:
            out.append(vocab[rnd(len(vocab))])
        elif roll == 2:
            out.append(src[i])
            out.append(vocab[rnd(len(vocab))])
        else:
            out.append(src[i])
        i += 1
    return out


def diff_counts(a, b):
    n1 = len(a)
    n2 = len(b)
    t = []
    i = 0
    while i <= n1:
        row = []
        j = 0
        while j <= n2:
            row.append(0)
            j += 1
        t.append(row)
        i += 1
    i = 1
    while i <= n1:
        j = 1
        while j <= n2:
            if a[i - 1] == b[j - 1]:
                t[i][j] = t[i - 1][j - 1] + 1
            else:
                up = t[i - 1][j]
                left = t[i][j - 1]
                if up >= left:
                    t[i][j] = up
                else:
                    t[i][j] = left
            j += 1
        i += 1
    eq = 0
    del_count = 0
    ins = 0
    i = n1
    j = n2
    while i > 0 and j > 0:
        if a[i - 1] == b[j - 1]:
            eq += 1
            i -= 1
            j -= 1
        elif t[i - 1][j] >= t[i][j - 1]:
            del_count += 1
            i -= 1
        else:
            ins += 1
            j -= 1
    del_count += i
    ins += j
    return eq, del_count, ins


t0 = time.perf_counter()
eq_sum = 0
del_sum = 0
ins_sum = 0
p = 0
while p < pairs:
    a = gen_doc()
    b = mutate(a)
    eq, del_count, ins = diff_counts(a, b)
    eq_sum = (eq_sum + eq) % 1000003
    del_sum = (del_sum + del_count) % 1000003
    ins_sum = (ins_sum + ins) % 1000003
    p += 1
dt = (time.perf_counter() - t0) * 1000.0

assert eq_sum == 4966, "word_diff eq_sum drift"
assert del_sum == 494, "word_diff del_sum drift"
assert ins_sum == 503, "word_diff ins_sum drift"


print("value eq_sum:", eq_sum)
print("value del_sum:", del_sum)
print("value ins_sum:", ins_sum)
print("bench-time:", dt, "ms")
