"""scope_locals 参照端口(与 scope_locals.aria 同算法同规模)。"""
import time


def work(a):
    b = a + 1
    c = b * 2
    d = c - 3
    out = d
    e = d + 4
    out = e % 1000003
    return out


rounds = 4500000

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    acc = (acc + work(i)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 250156, "scope_locals checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
