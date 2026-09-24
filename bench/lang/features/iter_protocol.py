"""iter_protocol 参照端口(与 iter_protocol.aria 同算法同规模)。"""
import time

size = 512
rounds = 4000

xs = list(range(size))
m = {k: k for k in range(size)}

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    for v in xs:
        acc = (acc + v) % 1000003
    for v in range(size):
        acc = (acc + v) % 1000003
    for _ch in "abc":
        acc = acc + 1
    for k in m:
        acc = (acc + k) % 1000003
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 799293, "iter_protocol checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
