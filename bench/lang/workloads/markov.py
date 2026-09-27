"""markov 参照端口(与 markov.aria 同算法同规模):二阶马尔可夫文本模型,LCG 造语料后训练
(词对 -> 后续词计数的交织平行数组,行内线性扫)再按计数加权采样生成。"""
import time

sentences = 6500
gen_words = 20000

vocab = ["the", "of", "and", "a", "to", "in", "is", "it", "was", "that", "for", "time",
         "world", "life", "mind", "light", "river", "stone", "forest", "winter", "summer",
         "harbor", "signal", "memory", "shadow", "window", "garden", "mirror", "voyage", "silence"]

state = 12345


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


t0 = time.perf_counter()

corpus = []
for _ in range(sentences):
    length = 6 + rnd(9)
    for _ in range(length):
        corpus.append(vocab[rnd(len(vocab))])

states = {}
w0 = corpus[0]
w1 = corpus[1]
for i in range(2, len(corpus)):
    key = w0 + " " + w1
    row = states.get(key)
    if row is None:
        row = []
        states[key] = row
    w2 = corpus[i]
    found = False
    p = 0
    while p < len(row):
        if row[p] == w2:
            row[p + 1] += 1
            found = True
        p += 2
    if not found:
        row.append(w2)
        row.append(1)
    w0 = w1
    w1 = w2

produced = []
checksum = 0
g0 = corpus[0]
g1 = corpus[1]
made = 0
while made < gen_words:
    row = states.get(g0 + " " + g1)
    if row is None:
        g0 = corpus[0]
        g1 = corpus[1]
        continue
    total = 0
    p = 1
    while p < len(row):
        total += row[p]
        p += 2
    pick = rnd(total)
    w = row[0]
    p = 1
    while p < len(row):
        if pick < row[p]:
            w = row[p - 1]
            break
        pick -= row[p]
        p += 2
    produced.append(w)
    checksum = (checksum * 31 + len(w)) % 1000003
    g0 = g1
    g1 = w
    made += 1

seen = set(produced)

assert len(states) == 900, "markov states drift"
assert checksum == 679528, "markov checksum drift"
assert len(seen) == 30, "markov distinct drift"

dt = (time.perf_counter() - t0) * 1000.0

print("value states:", len(states))
print("value checksum:", checksum)
print("value distinct:", len(seen))
print("bench-time:", dt, "ms")
