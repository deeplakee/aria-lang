-- route_planner 参照端口(与 route_planner.aria 同算法同规模):LCG 建带权有向图(邻接表),
-- 对全部源点跑朴素 Dijkstra(线性扫描取未定最小 + 松弛),累加各源点的距离和与可达数。
-- Lua 下标从 1 起:节点整体平移到 1..nodes_n,rnd 返回值 + 1 后参与比较与存储,读数不变。
local nodes_n = 120  -- 规模:路口数
local degree = 5     -- 每路口出边数

local state = 8080
local function rnd(n)
    state = (state * 48271) % 2147483647
    return state % n
end

local adj  -- 建表在计时段内,这里只先绑定局部名(dijkstra_sum 闭包捕获它)

local function dijkstra_sum(src)
    local dist = {}
    local done = {}
    for i = 1, nodes_n do
        dist[i] = -1
        done[i] = false
    end
    dist[src] = 0
    local total = 0
    local reached = 0
    while true do
        local u = 0  -- 0 是「未找到」哨兵;节点域是 1..nodes_n,不会撞上
        local best = 0
        for i = 1, nodes_n do
            if (not done[i]) and dist[i] >= 0 then
                if u == 0 or dist[i] < best then
                    u = i
                    best = dist[i]
                end
            end
        end
        if u == 0 then
            break
        end
        done[u] = true
        reached = reached + 1
        total = (total + best) % 1000003
        for _, e in ipairs(adj[u]) do
            local v = e[1]
            local w = e[2]
            if dist[v] < 0 or best + w < dist[v] then
                dist[v] = best + w
            end
        end
    end
    return total, reached
end

local t0 = os.clock()

adj = {}
for i = 1, nodes_n do
    local edges = {}
    for _ = 1, degree do
        local v = rnd(nodes_n) + 1
        if v ~= i then
            edges[#edges + 1] = {v, 1 + rnd(99)}
        end
    end
    adj[i] = edges
end

local total_sum = 0
local reach_sum = 0
for i = 1, nodes_n do
    local t, r = dijkstra_sum(i)
    total_sum = (total_sum + t) % 1000003
    reach_sum = reach_sum + r
end

assert(total_sum == 429150, "route_planner total_sum drift")
assert(reach_sum == 14281, "route_planner reach_sum drift")

local dt = (os.clock() - t0) * 1000.0

print("value total_sum: " .. total_sum)
print("value reach_sum: " .. reach_sum)
print("bench-time: " .. dt .. " ms")
