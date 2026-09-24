"""operator_overload 参照端口(与 operator_overload.aria 同算法同规模)。"""
import time


class Box:
    def __init__(self, v):
        self.v = v

    def __add__(self, other):
        return self.v + other.v


a = Box(3)
b = Box(4)
iterations = 4000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + (a + b)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999919, "operator_overload checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
