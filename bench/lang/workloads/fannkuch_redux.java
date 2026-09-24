// fannkuch_redux 参照端口(与 fannkuch_redux.aria 同算法同规模):每轮像 aria 一样复制一份待翻面
// 的副本(perm.clone()),故分配行为也对应。
class Bench_fannkuch_redux {
    static long[] pass() {
        int n = 9;
        int[] perm = new int[n];
        for (int i = 0; i < n; i++) {
            perm[i] = i;
        }
        long maxFlips = 0;
        long flipsSum = 0;
        long permCount = 0;
        while (true) {
            if (perm[0] != 0) {
                int[] work = perm.clone();
                long flips = 0;
                while (work[0] != 0) {
                    int k = work[0] + 1;
                    for (int left = 0, right = k - 1; left < right; left++, right--) {
                        int tmp = work[left];
                        work[left] = work[right];
                        work[right] = tmp;
                    }
                    flips += 1;
                }
                if (flips > maxFlips) {
                    maxFlips = flips;
                }
                flipsSum += flips;
                permCount += 1;
            }
            int i = n - 2;
            while (i >= 0 && perm[i] >= perm[i + 1]) {
                i -= 1;
            }
            if (i < 0) {
                break;
            }
            int j = n - 1;
            while (perm[j] <= perm[i]) {
                j -= 1;
            }
            int tmp2 = perm[i];
            perm[i] = perm[j];
            perm[j] = tmp2;
            for (int left = i + 1, right = n - 1; left < right; left++, right--) {
                int tmp3 = perm[left];
                perm[left] = perm[right];
                perm[right] = tmp3;
            }
        }
        return new long[] {flipsSum, maxFlips, permCount};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm[0], v[0]);
        check("checksum", v[0], 1911505L);
        check("max_flips", v[1], 30L);
        check("perm_count", v[2], 322560L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value max_flips: " + v[1]);
        System.out.println("value perm_count: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
