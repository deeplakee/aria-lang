"""list_methods 参照端口(与 list_methods.aria 同算法同规模)。"""
import time

size = 2000
rounds = 800000

xs = list(range(size))

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    xs.append(r)
    xs.insert(0, r)
    if r in xs:
        acc = (acc + xs.index(r)) % 1000003
    acc = (acc + xs.pop(2)) % 1000003
    acc = (acc + xs.pop()) % 1000003
    acc = (acc + len(xs)) % 1000003
    r += 1
joined = ",".join(str(v) for v in xs)
acc = (acc + len(joined)) % 1000003
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 684118, "list_methods checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
