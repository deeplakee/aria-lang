"""map_ops 参照端口(与 map_ops.aria 同算法同规模)。"""
import time

entries = 100000
rounds = 40

m = {}
i = 0
while i < entries:
    m[i] = i * 2
    i += 1

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    j = 0
    while j < entries:
        if j in m:
            acc = (acc + m[j]) % 1000003
        j += 1
    r += 1
acc = (acc + len(m)) % 1000003
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 900018, "map_ops checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
