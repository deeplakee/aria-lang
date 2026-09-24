"""fannkuch_redux 参照端口(与 fannkuch_redux.aria 同算法同规模):字典序枚举全部置换,对首元素
非 0 者统计 pancake 翻面次数。公开的带符号 checksum 约定无法从公开说明复原,故钉三个序无关量:
max_flips(与公开 Pfannkuchen 值可比)、flips_sum(本文自定义求和)、perm_count(闭式 n!-(n-1)!)。"""
import time

n = 9
fact_n = 362880
fact_nm1 = 40320

t0 = time.perf_counter()
perm = list(range(n))
max_flips = 0
flips_sum = 0
perm_count = 0
while True:
    if perm[0] != 0:
        work = list(perm)
        flips = 0
        while work[0] != 0:
            k = work[0] + 1
            left = 0
            right = k - 1
            while left < right:
                tmp = work[left]
                work[left] = work[right]
                work[right] = tmp
                left += 1
                right -= 1
            flips += 1
        if flips > max_flips:
            max_flips = flips
        flips_sum += flips
        perm_count += 1
    i = n - 2
    while i >= 0 and perm[i] >= perm[i + 1]:
        i -= 1
    if i < 0:
        break
    j = n - 1
    while perm[j] <= perm[i]:
        j -= 1
    perm[i], perm[j] = perm[j], perm[i]
    left = i + 1
    right = n - 1
    while left < right:
        perm[left], perm[right] = perm[right], perm[left]
        left += 1
        right -= 1
dt = (time.perf_counter() - t0) * 1000.0

assert flips_sum == 1911505, "fannkuch_redux flips_sum drift"
assert max_flips == 30, "fannkuch_redux max_flips drift"
assert perm_count == 322560, "fannkuch_redux perm_count drift"


print("value checksum:", flips_sum)
print("value max_flips:", max_flips)
print("value perm_count:", perm_count)
print("bench-time:", dt, "ms")
