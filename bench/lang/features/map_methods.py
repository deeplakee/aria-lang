"""map_methods 参照端口(与 map_methods.aria 同算法同规模):快照序 unspecified,只做与序无关的
求和与计数(与 aria 端同一算法)。"""
import time

entries = 20000
rounds = 1600

m = {}
i = 0
while i < entries:
    m[i] = i * 2
    i += 1

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    key = r % entries
    m.pop(key, None)
    m[key] = r
    if key in m:
        acc = (acc + m[key]) % 1000003
    acc = (acc + len(m)) % 1000003
    if r % 10 == 0:
        ks = list(m.keys())
        vs = list(m.values())
        ps = list(m.items())
        acc = (acc + len(ks) + len(vs) + len(ps)) % 1000003
        j = 0
        while j < len(ks):
            acc = (acc + ks[j]) % 1000003
            j += 1
    r += 1
m.clear()
acc = (acc + len(m)) % 1000003
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 183077, "map_methods checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
