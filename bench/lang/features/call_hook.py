"""call_hook 参照端口(与 call_hook.aria 同算法同规模)。"""
import time


class Adder:
    def __init__(self, k):
        self.k = k

    def __call__(self, x):
        return self.k + x


add2 = Adder(2)
iterations = 6000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + add2(i)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 135, "call_hook checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
