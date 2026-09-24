"""compare_branch 参照端口(与 compare_branch.aria 同算法同规模)。"""
import time

iterations = 4000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    if i % 15 == 0:
        acc += 3
    elif i % 5 == 0:
        acc += 2
    elif i % 3 == 0:
        acc += 1
    else:
        acc += 0
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 2933334, "compare_branch checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
