"""class_field_rw 参照端口(与 class_field_rw.aria 同算法同规模)。"""
import time


class Cell:
    def __init__(self):
        self.v = 0


cell = Cell()
iterations = 7000000

t0 = time.perf_counter()
i = 0
while i < iterations:
    cell.v = cell.v + 1
    i += 1
acc = cell.v
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 7000000, "class_field_rw checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
