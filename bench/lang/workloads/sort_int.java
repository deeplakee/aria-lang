// sort_int 参照端口(与 sort_int.aria 同算法同规模)。
class Bench_sort_int {
    static long[] pass() {
        int n = 2500000;
        long[] xs = new long[n];
        long state = 1;
        for (int i = 0; i < n; i++) {
            state = (state * 48271) % 2147483647L;
            xs[i] = state;
        }
        java.util.Arrays.sort(xs);
        long checksum = 0;
        for (int i = 0; i < n; i++) {
            checksum = (checksum * 31 + xs[i] % 1009) % 1000000007L;
        }
        return new long[] {checksum, xs[0], xs[n - 1]};
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
        check("checksum", v[0], 947379667L);
        check("min", v[1], 145L);
        check("max", v[2], 2147483426L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value min: " + v[1]);
        System.out.println("value max: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
