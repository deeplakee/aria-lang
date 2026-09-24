"""closure_creation 参照端口(与 closure_creation.aria 同算法同规模)。"""
import time


def make(base):
    return lambda d: base + d


iterations = 2800000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    f = make(i)
    acc = (acc + f(2)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 440024, "closure_creation checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
