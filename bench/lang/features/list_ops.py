"""list_ops 参照端口(与 list_ops.aria 同算法同规模)。"""
import time

size = 200000
rounds = 20

xs = []
i = 0
while i < size:
    xs.append(i)
    i += 1

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    j = 0
    while j < size:
        acc = (acc + xs[j] + xs[size - 1 - j]) % 1000003
        xs[j] = (xs[j] + 1) % 1000003
        j += 1
    r += 1
acc = (acc + len(xs)) % 1000003
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 799787, "list_ops checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
