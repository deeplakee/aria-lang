"""string_ops 参照端口(与 string_ops.aria 同算法同规模)。"""
import time

rounds = 1000000
base = "the quick brown fox jumps over the lazy dog"

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    if "brown" in base:
        acc += base.index("fox")
    parts = base.split(" ")
    acc = (acc + len(parts) + len(base[4:9])) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999913, "string_ops checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
