"""sieve 参照端口(与 sieve.aria 同算法同规模):埃拉托斯特尼筛三个规模,钉素数个数。"""
import time

t0 = time.perf_counter()
counts = []
for n in (100000, 1000000, 4000000):
    composite = bytearray(n)
    count = 0
    i = 2
    while i < n:
        if composite[i] == 0:
            count += 1
            j = i * i
            while j < n:
                composite[j] = 1
                j += i
        i += 1
    counts.append(count)
    print("value count_%d: %d" % (n, count))
dt = (time.perf_counter() - t0) * 1000.0

assert counts[0] == 9592, "sieve count_100000 drift"
assert counts[1] == 78498, "sieve count_1000000 drift"
assert counts[2] == 283146, "sieve count_4000000 drift"
assert sum(counts) == 371236, "sieve total drift"

print("value checksum:", sum(counts))
print("bench-time:", dt, "ms")
