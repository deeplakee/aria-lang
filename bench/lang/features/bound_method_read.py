"""bound_method_read 参照端口(与 bound_method_read.aria 同算法同规模):每轮读一次 obj.step,
Python 的绑定方法对象创建与 aria 的现场绑定同为「读路径分配」。"""
import time


class Cell:
    def __init__(self, v):
        self.v = v

    def step(self, d):
        return self.v + d


obj = Cell(1)
iterations = 3800000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    f = obj.step
    acc = (acc + f(i)) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 240060, "bound_method_read checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
