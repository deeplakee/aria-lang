"""arith_int_loop 参照端口(与 arith_int_loop.aria 同算法同规模)。"""
import time

iterations = 5000000

t0 = time.perf_counter()
acc = 1
i = 0
while i < iterations:
    acc = (acc + i) % 1000003
    acc = acc * 3 - 1
    acc += i % 7
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 109608, "arith_int_loop checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
