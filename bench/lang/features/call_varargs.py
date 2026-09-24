"""call_varargs 参照端口(与 call_varargs.aria 同算法同规模)。"""
import time


def total(*xs):
    s = 0
    for v in xs:
        s = s + v
    return s


iterations = 1300000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + total(i, i + 1, i + 2)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 345018, "call_varargs checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
