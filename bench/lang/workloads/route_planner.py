"""route_planner 参照端口(与 route_planner.aria 同算法同规模):LCG 建带权有向图(邻接表),
对全部源点跑朴素 Dijkstra(线性扫描取未定最小 + 松弛),累加各源点的距离和与可达数。"""
import time

nodes_n = 120  # 规模:路口数
degree = 5     # 每路口出边数

state = 8080


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


def dijkstra_sum(src):
    dist = [-1] * nodes_n
    done = [False] * nodes_n
    dist[src] = 0
    total = 0
    reached = 0
    while True:
        u = -1
        best = 0
        i = 0
        while i < nodes_n:
            if not done[i] and dist[i] >= 0:
                if u < 0 or dist[i] < best:
                    u = i
                    best = dist[i]
            i += 1
        if u < 0:
            break
        done[u] = True
        reached += 1
        total = (total + best) % 1000003
        for e in adj[u]:
            v = e[0]
            w = e[1]
            if dist[v] < 0 or best + w < dist[v]:
                dist[v] = best + w
    return total, reached


t0 = time.perf_counter()

adj = []
i = 0
while i < nodes_n:
    edges = []
    j = 0
    while j < degree:
        v = rnd(nodes_n)
        if v != i:
            edges.append([v, 1 + rnd(99)])
        j += 1
    adj.append(edges)
    i += 1

total_sum = 0
reach_sum = 0
i = 0
while i < nodes_n:
    t, r = dijkstra_sum(i)
    total_sum = (total_sum + t) % 1000003
    reach_sum += r
    i += 1

assert total_sum == 429150, "route_planner total_sum drift"
assert reach_sum == 14281, "route_planner reach_sum drift"

dt = (time.perf_counter() - t0) * 1000.0

print("value total_sum:", total_sum)
print("value reach_sum:", reach_sum)
print("bench-time:", dt, "ms")
