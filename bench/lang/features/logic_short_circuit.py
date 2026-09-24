"""logic_short_circuit 参照端口(与 logic_short_circuit.aria 同算法同规模)。"""
import time

iterations = 6000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < iterations:
    if (i > 0 and i % 2 == 0) or i % 7 == 0:
        acc = acc + 1
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 3428571, "logic_short_circuit checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
