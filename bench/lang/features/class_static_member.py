"""class_static_member 参照端口(与 class_static_member.aria 同算法同规模)。"""
import time


class Counter:
    n = 0

    @staticmethod
    def add(d):
        Counter.n = Counter.n + d
        return Counter.n


iterations = 4200000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + Counter.add(1)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 640075, "class_static_member checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
