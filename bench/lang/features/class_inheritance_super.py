"""class_inheritance_super 参照端口(与 class_inheritance_super.aria 同算法同规模)。"""
import time


class Base:
    def __init__(self, v):
        self.v = v

    def describe(self):
        return 1


class Derived(Base):
    def __init__(self, v):
        super().__init__(v)

    def describe(self):
        return 10 + super().describe()


obj = Derived(7)
iterations = 4000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + obj.describe()) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999871, "class_inheritance_super checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
