"""recursion_fib 参照端口(与 recursion_fib.aria 同算法同规模)。"""
import time


def fib(n):
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)


rounds = 20
fib_input = 27

t0 = time.perf_counter()
acc = 0
r = 0
while r < rounds:
    acc += fib(fib_input)
    r += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 3928360, "recursion_fib checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
