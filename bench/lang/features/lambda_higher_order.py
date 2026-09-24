"""lambda_higher_order 参照端口(与 lambda_higher_order.aria 同算法同规模)。"""
import time


def compose(f, g):
    return lambda x: f(g(x))


inc = lambda x: x + 1
dbl = lambda x: x * 2
chain = compose(inc, dbl)
iterations = 5000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    acc = chain(acc) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 395508, "lambda_higher_order checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
