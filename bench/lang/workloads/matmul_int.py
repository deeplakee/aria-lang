"""matmul_int 参照端口(与 matmul_int.aria 同算法同规模):嵌套 list 的整数矩阵乘,量下标密度。"""
import time

n = 250

t0 = time.perf_counter()
a = []
i = 0
while i < n:
    row = []
    j = 0
    while j < n:
        row.append((i * 7 + j * 13 + 1) % 1009)
        j += 1
    a.append(row)
    i += 1
b = []
i = 0
while i < n:
    row = []
    j = 0
    while j < n:
        row.append((i * 11 + j * 3 + 2) % 1009)
        j += 1
    b.append(row)
    i += 1

checksum = 0
i = 0
while i < n:
    j = 0
    while j < n:
        acc = 0
        k = 0
        while k < n:
            acc += a[i][k] * b[k][j]
            k += 1
        checksum = (checksum * 31 + acc) % 1000000007
        j += 1
    i += 1
dt = (time.perf_counter() - t0) * 1000.0

assert checksum == 396310743, "matmul_int checksum drift"


print("value checksum:", checksum)
print("bench-time:", dt, "ms")
