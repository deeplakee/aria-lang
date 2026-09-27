// route_planner 参照端口(与 route_planner.aria 同算法同规模):LCG 建带权有向图(邻接表),
// 对全部源点跑朴素 Dijkstra(线性扫描取未定最小 + 松弛),累加各源点的距离和与可达数。
class Bench_route_planner {
    static long state = 8080;

    static long rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return state % n;
    }

    static long[] dijkstraSum(int[][][] adj, int src, int nodesN) {
        int[] dist = new int[nodesN];
        boolean[] done = new boolean[nodesN];
        java.util.Arrays.fill(dist, -1);
        dist[src] = 0;
        long total = 0;
        int reached = 0;
        for (;;) {
            int u = -1;
            int best = 0;
            for (int i = 0; i < nodesN; i++) {
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
            total = (total + best) % 1000003L;
            for (int[] e : adj[u]) {
                int v = e[0];
                int w = e[1];
                if (dist[v] < 0 || best + w < dist[v]) {
                    dist[v] = best + w;
                }
            }
        }
        return new long[] {total, reached};
    }

    static long[] pass() {
        state = 8080;  // LCG 种子每遍重置:warmup 与计时两遍产出同一张图
        int nodesN = 120;  // 规模:路口数
        int degree = 5;    // 每路口出边数
        int[][][] adj = new int[nodesN][][];
        for (int i = 0; i < nodesN; i++) {
            int[][] edges = new int[degree][];
            int count = 0;
            for (int j = 0; j < degree; j++) {
                int v = (int) rnd(nodesN);
                if (v != i) {
                    edges[count] = new int[] {v, 1 + (int) rnd(99)};
                    count++;
                }
            }
            adj[i] = java.util.Arrays.copyOf(edges, count);
        }
        long totalSum = 0;
        long reachSum = 0;
        for (int i = 0; i < nodesN; i++) {
            long[] tr = dijkstraSum(adj, i, nodesN);
            totalSum = (totalSum + tr[0]) % 1000003L;
            reachSum += tr[1];
        }
        return new long[] {totalSum, reachSum};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("value drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup total_sum stable", warm[0], v[0]);
        check("warmup reach_sum stable", warm[1], v[1]);
        check("total_sum", v[0], 429150L);
        check("reach_sum", v[1], 14281L);
        System.out.println("value total_sum: " + v[0]);
        System.out.println("value reach_sum: " + v[1]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
