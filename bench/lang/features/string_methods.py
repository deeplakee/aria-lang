"""string_methods 参照端口(与 string_methods.aria 同算法同规模)。"""
import time

rounds = 450000
base = "  the quick brown fox jumps over the lazy dog  "

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    t = base.strip()
    parts = t.split(" ")
    joined = "-".join(parts)
    up = t.upper()
    if t.startswith("the") and t.endswith("dog"):
        acc = acc + 1
    acc = (acc + t.index("fox") + len(t.replace("o", "0")) + len(joined) + len(up) +
           len(t[4:9])) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 949799, "string_methods checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
