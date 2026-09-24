"""list_sort_reverse 参照端口(与 list_sort_reverse.aria 同算法同规模)。"""
import time

size = 200000
rounds = 20

xs = []
state = 1
i = 0
while i < size:
    state = (state * 48271) % 2147483647
    xs.append(state)
    i += 1

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    xs.sort()
    xs.reverse()
    j = 0
    while j < size:
        acc = (acc * 31 + xs[j] % 1009) % 1000000007
        j += 1
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 745687805, "list_sort_reverse checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
