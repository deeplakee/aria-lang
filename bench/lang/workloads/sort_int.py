"""sort_int 参照端口(与 sort_int.aria 同算法同规模):Park-Miller LCG 造数 -> 就地排序 ->
滚动校验和(排序后序确定,故校验和确定)。"""
import time

n = 2500000

t0 = time.perf_counter()
xs = []
state = 1
i = 0
while i < n:
    state = (state * 48271) % 2147483647
    xs.append(state)
    i += 1
xs.sort()

checksum = 0
i = 0
while i < n:
    checksum = (checksum * 31 + xs[i] % 1009) % 1000000007
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert checksum == 947379667, "sort_int checksum drift"
assert xs[0] == 145, "sort_int min drift"
assert xs[n - 1] == 2147483426, "sort_int max drift"


print("value checksum:", checksum)
print("value min:", xs[0])
print("value max:", xs[n - 1])
print("bench-time:", dt, "ms")
