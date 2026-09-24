"""destructure_slice 参照端口(与 destructure_slice.aria 同算法同规模)。"""
import time

iterations = 2000000
pair = [1, 2]
xs = [10, 20, 30, 40, 50]

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    a, b = pair
    head = xs[1:4]
    x, y = head[0], head[1]
    acc = (acc + a + b + x + y) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999685, "destructure_slice checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
