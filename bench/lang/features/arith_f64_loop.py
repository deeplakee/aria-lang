"""arith_f64_loop 参照端口(与 arith_f64_loop.aria 同算法同规模)。"""
import time

iterations = 5000000

t0 = time.perf_counter()
x = 0.5
hits = 0
i = 0
while i < iterations:
    x = x * 1.0000001 + 0.25
    if x > 100.0:
        x = x / 2.0
    if x > 50.0:
        hits += 1
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

print("value checksum:", hits)
print("bench-time:", dt, "ms")
