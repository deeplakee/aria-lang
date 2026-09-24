// list_sort_reverse 参照端口(与 list_sort_reverse.aria 同算法同规模):原生数组排序 + 手写就地反转。
class Bench_list_sort_reverse {
    static long pass() {
        int size = 200000;
        long[] xs = new long[size];
        long state = 1;
        for (int i = 0; i < size; i++) {
            state = (state * 48271) % 2147483647L;
            xs[i] = state;
        }
        long acc = 0;
        for (int r = 0; r < 20; r++) {
            java.util.Arrays.sort(xs);
            for (int lo = 0, hi = size - 1; lo < hi; lo++, hi--) {
                long tmp = xs[lo];
                xs[lo] = xs[hi];
                xs[hi] = tmp;
            }
            for (int j = 0; j < size; j++) {
                acc = (acc * 31 + xs[j] % 1009) % 1000000007L;
            }
        }
        return acc;
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long warm = pass();
        long t0 = System.nanoTime();
        long acc = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm, acc);
        check("checksum", acc, 745687805L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
