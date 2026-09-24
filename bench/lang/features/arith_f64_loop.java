// arith_f64_loop 参照端口(与 arith_f64_loop.aria 同算法同规模)。
class Bench_arith_f64_loop {
    static long pass() {
        double x = 0.5;
        long hits = 0;
        long i = 0;
        while (i < 5000000L) {
            x = x * 1.0000001 + 0.25;
            if (x > 100.0) {
                x = x / 2.0;
            }
            if (x > 50.0) {
                hits += 1;
            }
            i += 1;
        }
        return hits;
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
        check("checksum", acc, 4999803L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
