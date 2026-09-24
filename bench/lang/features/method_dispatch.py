"""method_dispatch 参照端口(与 method_dispatch.aria 同算法同规模)。"""
import time


class Counter:
    def __init__(self):
        self.base = 1

    def step(self, delta):
        return self.base + delta


counter = Counter()
iterations = 5000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + counter.step(i)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 105, "method_dispatch checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
