"""forin_destructure 参照端口(与 forin_destructure.aria 同算法同规模)。"""
import time

size = 256
rounds = 13000

m = {k: k * 2 for k in range(size)}

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    for k, v in m.items():
        acc = (acc + k + v) % 1000003
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 956184, "forin_destructure checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
