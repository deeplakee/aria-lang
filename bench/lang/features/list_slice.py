"""list_slice 参照端口(与 list_slice.aria 同算法同规模):aria 的区间切片两端皆含,
故 Python 端用 [start:stop+1](倒序段用负步长,stop 亦为「上界+1」)。"""
import time

size = 512
rounds = 900000

xs = list(range(size))

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    a = xs[10:21]
    b = xs[-20:]
    c = xs[20:9:-1]
    acc = (acc + len(a) + len(b) + len(c) + a[0] + b[0] + c[0]) % 1000003
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 598479, "list_slice checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
