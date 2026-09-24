"""call_default_params 参照端口(与 call_default_params.aria 同算法同规模)。"""
import time


def step(v, k=3):
    return v + k


iterations = 5500000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = (acc + step(i)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 375096, "call_default_params checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
