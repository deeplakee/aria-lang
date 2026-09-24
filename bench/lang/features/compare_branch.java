// compare_branch 参照端口(与 compare_branch.aria 同算法同规模)。
class Bench_compare_branch {
    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 4000000L) {
            if (i % 15 == 0) {
                acc += 3;
            } else if (i % 5 == 0) {
                acc += 2;
            } else if (i % 3 == 0) {
                acc += 1;
            } else {
                acc += 0;
            }
            i += 1;
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
        check("checksum", acc, 2933334L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
