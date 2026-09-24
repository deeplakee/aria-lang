"""mandelbrot 参照端口(与 mandelbrot.aria 同算法同规模):每像素最多 max_iter 次复平面迭代,
校验和 = 逐像素迭代数滚动取模(避开 aria 无移位算子),另钉内部点计数与像素总数。"""
import time

size = 500
max_iter = 50

t0 = time.perf_counter()
checksum = 0
inside = 0
py_idx = 0
while py_idx < size:
    y0 = py_idx * 1.0 / size * 2.0 - 1.0
    px = 0
    while px < size:
        x0 = px * 1.0 / size * 2.5 - 2.0
        x = 0.0
        y = 0.0
        i = 0
        while i < max_iter:
            x2 = x * x
            y2 = y * y
            if x2 + y2 > 4.0:
                break
            y = 2.0 * x * y + y0
            x = x2 - y2 + x0
            i += 1
        checksum = (checksum * 2 + i) % 1000000007
        if i == max_iter:
            inside += 1
        px += 1
    py_idx += 1
dt = (time.perf_counter() - t0) * 1000.0

assert checksum == 140394512, "mandelbrot checksum drift"
assert inside == 79596, "mandelbrot inside drift"
assert size * size == 250000, "mandelbrot pixels drift"


print("value checksum:", checksum)
print("value inside:", inside)
print("value pixels:", size * size)
print("bench-time:", dt, "ms")
