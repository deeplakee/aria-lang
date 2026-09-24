"""plain_call 参照端口(与 plain_call.aria 同算法同规模)。"""
import time


def step(v):
    return v + 1


iterations = 6000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = acc + step(i)
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 18000003000000, "plain_call checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
