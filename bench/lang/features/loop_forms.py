"""loop_forms 参照端口(与 loop_forms.aria 同算法同规模)。"""
import time

rounds = 500000

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    for k in range(16):
        if k % 3 == 0:
            continue
        if k > 12:
            break
        acc = (acc + k) % 1000003
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999931, "loop_forms checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
