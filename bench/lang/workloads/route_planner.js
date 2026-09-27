// route_planner 参照端口(与 route_planner.aria 同算法同规模):LCG 建带权有向图(邻接表),
// 对全部源点跑朴素 Dijkstra(线性扫描取未定最小 + 松弛),累加各源点的距离和与可达数。
const nodes_n = 120;  // 规模:路口数
const degree = 5;     // 每路口出边数

let state = 8080;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

function dijkstra_sum(src) {
    const dist = new Array(nodes_n).fill(-1);
    const done = new Array(nodes_n).fill(false);
    dist[src] = 0;
    let total = 0;
    let reached = 0;
    for (;;) {
        let u = -1;
        let best = 0;
        for (let i = 0; i < nodes_n; i++) {
            if (!done[i] && dist[i] >= 0) {
                if (u < 0 || dist[i] < best) {
                    u = i;
                    best = dist[i];
                }
            }
        }
        if (u < 0) {
            break;
        }
        done[u] = true;
        reached++;
        total = (total + best) % 1000003;
        for (const e of adj[u]) {
            const v = e[0];
            const w = e[1];
            if (dist[v] < 0 || best + w < dist[v]) {
                dist[v] = best + w;
            }
        }
    }
    return [total, reached];
}

const t0 = performance.now();

const adj = [];
for (let i = 0; i < nodes_n; i++) {
    const edges = [];
    for (let j = 0; j < degree; j++) {
        const v = rnd(nodes_n);
        if (v !== i) {
            edges.push([v, 1 + rnd(99)]);
        }
    }
    adj.push(edges);
}

let total_sum = 0;
let reach_sum = 0;
for (let i = 0; i < nodes_n; i++) {
    const [t, r] = dijkstra_sum(i);
    total_sum = (total_sum + t) % 1000003;
    reach_sum += r;
}

check("route_planner.total_sum", total_sum, 429150);
check("route_planner.reach_sum", reach_sum, 14281);

const dt = performance.now() - t0;

console.log("value total_sum: " + total_sum);
console.log("value reach_sum: " + reach_sum);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("value drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
