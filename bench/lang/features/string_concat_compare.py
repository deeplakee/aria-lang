"""string_concat_compare 参照端口(与 string_concat_compare.aria 同算法同规模)。"""
import time

rounds = 2000000

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    s = "abc" + "def" + "ghi"
    if s == "abcdefghi":
        acc = acc + 1
    if s < "abz":
        acc = acc + 2
    if s >= "abcdefgh":
        acc = acc + 3
    acc = (acc + len(s + "!")) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 999907, "string_concat_compare checksum drift"
print("value checksum:", acc)
print("bench-time:", dt, "ms")
