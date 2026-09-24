"""word_count 参照端口(与 word_count.aria 同算法同规模):LCG 造词表文本 -> join -> split -> map 计数。"""
import time

vocab = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"]
words = 3000000

t0 = time.perf_counter()
state = 1
parts = []
i = 0
while i < words:
    state = (state * 48271) % 2147483647
    parts.append(vocab[state % 8])
    i += 1
text = " ".join(parts)

counts = {}
for w in text.split(" "):
    if w in counts:
        counts[w] = counts[w] + 1
    else:
        counts[w] = 1

total = 0
for w in vocab:
    total += counts[w]
dt = (time.perf_counter() - t0) * 1000.0

assert total == 3000000, "word_count total drift"
assert counts["alpha"] == 374756, "word_count alpha drift"
assert counts["theta"] == 374622, "word_count theta drift"


print("value checksum:", total)
print("value alpha:", counts["alpha"])
print("value theta:", counts["theta"])
print("bench-time:", dt, "ms")
