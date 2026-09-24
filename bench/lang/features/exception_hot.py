"""exception_hot 参照端口(与 exception_hot.aria 同算法同规模)。

Python 不能 throw 任意值(raise 只收 BaseException 派生),故用一个携带数值的自定义异常,
其余同 aria:抛出、catch 住、把载荷累加进校验和。
"""
import time


class BenchError(Exception):
    def __init__(self, v):
        super().__init__(v)
        self.v = v


iterations = 8000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    try:
        raise BenchError(i)
    except BenchError as e:
        acc = (acc + e.v) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 300, "exception_hot checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
