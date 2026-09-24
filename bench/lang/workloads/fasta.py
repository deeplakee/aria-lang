"""fasta 参照端口(与 fasta.aria 同算法同规模):LCG 造碱基序列(累积概率选择),join 成整串后按
字符滚动取模校验;不含经典版的行宽折行(校验和定义在原始序列上)。"""
import time

im = 139968
ia = 3877
ic = 29573
state = 42

chars = ["a", "c", "g", "t", "B", "D", "H", "K", "M", "N", "R", "S", "V", "W", "Y"]
probs = [0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02]
cum = []
running = 0.0
for p in probs:
    running = running + p
    cum.append(running)

bases = 2000000

t0 = time.perf_counter()
parts = []
checksum = 0
i = 0
while i < bases:
    state = (state * ia + ic) % im
    pick = state * 1.0 / im
    idx = 0
    while idx < 15 and cum[idx] <= pick:
        idx += 1
    if idx == 15:
        idx = 14
    parts.append(chars[idx])
    checksum = (checksum * 131 + ord(chars[idx])) % 1000000007
    i += 1
seq = "".join(parts)
dt = (time.perf_counter() - t0) * 1000.0

assert checksum == 575313243, "fasta checksum drift"
assert len(seq) == 2000000, "fasta length drift"


print("value checksum:", checksum)
print("value length:", len(seq))
print("bench-time:", dt, "ms")
