"""exception_deep_unwind 参照端口(与 exception_deep_unwind.aria 同算法同规模)。"""
import time


class BenchError(Exception):
    def __init__(self, v):
        super().__init__(v)
        self.v = v


def level3(v):
    raise BenchError(v)


def level2(v):
    level3(v)
    return -1


def level1(v):
    level2(v)
    return -1


iterations = 2600000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    try:
        level1(i)
    except BenchError as e:
        acc = (acc + e.v) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 560036, "exception_deep_unwind checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
