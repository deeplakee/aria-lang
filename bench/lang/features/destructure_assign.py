"""destructure_assign 参照端口(与 destructure_assign.aria 同算法同规模)。"""
import time

rounds = 1200000

p = 1
q = 2

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    p, q = q, p + q
    p = p % 1000003
    q = q % 1000003
    x, *rest = (p, q, p + q)
    acc = (acc + x + len(rest)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 180840, "destructure_assign checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
