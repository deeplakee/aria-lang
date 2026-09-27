"""bank_queue 参照端口(与 bank_queue.aria 同算法同规模)。"""
import time

customers_n = 78000
key_unit = 1000000

state = 424242


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


class Teller:
    def __init__(self):
        self.busy_until = 0
        self.served = 0


tellers = []
ti = 0
while ti < 4:
    tellers.append(Teller())
    ti += 1

heap = []
queue = []
seq = 0
served = 0
wait_sum = 0
last_time = 0


def heap_push(entry):
    heap.append(entry)
    i = len(heap) - 1
    while i > 0:
        parent = (i - 1) // 2
        if heap[parent][0] <= heap[i][0]:
            break
        tmp = heap[parent]
        heap[parent] = heap[i]
        heap[i] = tmp
        i = parent


def heap_pop():
    top = heap[0]
    last = heap.pop()
    if len(heap) > 0:
        heap[0] = last
        i = 0
        n = len(heap)
        while True:
            l = 2 * i + 1
            r = l + 1
            m = i
            if l < n and heap[l][0] < heap[m][0]:
                m = l
            if r < n and heap[r][0] < heap[m][0]:
                m = r
            if m == i:
                break
            tmp = heap[m]
            heap[m] = heap[i]
            heap[i] = tmp
            i = m
    return top


def start_service(idx, now):
    global served, seq
    served += 1
    tellers[idx].served += 1
    svc = 20 + rnd(60)
    tellers[idx].busy_until = now + svc
    seq += 1
    heap_push([(now + svc) * key_unit + seq, 1, idx])


def on_arrival(key):
    global last_time, seq
    now = key // key_unit
    last_time = now
    nxt = now + 1 + rnd(40)
    seq += 1
    heap_push([nxt * key_unit + seq, 0, 0])
    free = -1
    k = 0
    while k < len(tellers):
        if tellers[k].busy_until <= now:
            free = k
        k += 1
    if free >= 0:
        start_service(free, now)
    else:
        queue.append(now)


def on_done(key, idx):
    global last_time, wait_sum
    now = key // key_unit
    last_time = now
    if len(queue) > 0:
        arrive = queue.pop(0)
        wait_sum += now - arrive
        start_service(idx, now)


t0 = time.perf_counter()

seq += 1
heap_push([(1 + rnd(40)) * key_unit + seq, 0, 0])
while served < customers_n:
    ev = heap_pop()
    if ev[1] == 0:
        on_arrival(ev[0])
    else:
        on_done(ev[0], ev[2])

load_sum = 0
k2 = 0
while k2 < len(tellers):
    load_sum += tellers[k2].served
    k2 += 1

assert served == customers_n, "bank_queue served drift"
assert load_sum == served, "bank_queue load_sum drift"
assert wait_sum == 101322, "bank_queue wait_sum drift"
assert last_time == 1604597, "bank_queue last_time drift"

dt = (time.perf_counter() - t0) * 1000.0


print("value served:", served)
print("value wait_sum:", wait_sum)
print("value last_time:", last_time)
print("value load_sum:", load_sum)
print("bench-time:", dt, "ms")
