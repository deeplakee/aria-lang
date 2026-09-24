"""match_dispatch 参照端口(与 match_dispatch.aria 同算法同规模)。"""
import time

iterations = 4000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    match i % 5:
        case 0:
            bucket = 3
        case 1:
            bucket = 5
        case 2:
            bucket = 7
        case 3:
            bucket = 11
        case _:
            bucket = 13
    acc = (acc + bucket) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 199907, "match_dispatch checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
