"""string_codepoint 参照端口(与 string_codepoint.aria 同算法同规模):码点域;字节域口径各语言
不同,不进校验和。"""
import time

rounds = 1100000
base = "héllo wörld"

t0 = time.perf_counter()
acc = 0
i = 0
while i < rounds:
    cs = list(base)
    acc = (acc + len(cs) + ord(base[1]) + ord(base[4]) + ord(cs[0])) % 1000003
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert acc == 898488, "string_codepoint checksum drift"
assert len(base) == 11, "string_codepoint codepoints drift"
print("value checksum:", acc)
print("value codepoints:", len(base))
print("bench-time:", dt, "ms")
