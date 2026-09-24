"""closure_upvalue 参照端口(与 closure_upvalue.aria 同算法同规模)。"""
import time


def make_counter():
    n = 0

    def step():
        nonlocal n
        n = n + 1
        return n

    return step


next_value = make_counter()
iterations = 6000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = acc + next_value()
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 18000003000000, "closure_upvalue checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
