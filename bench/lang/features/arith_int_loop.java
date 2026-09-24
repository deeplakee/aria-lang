// arith_int_loop 参照端口(与 arith_int_loop.aria 同算法同规模)。
// JVM 惯例:先跑一遍热身丢弃(触发 JIT),再计时跑第二遍--见 lang-bench-notes.md「端口纪律」。
class Bench_arith_int_loop {
    static long pass() {
        long acc = 1;
        long i = 0;
        while (i < 5000000L) {
            acc = (acc + i) % 1000003L;
            acc = acc * 3 - 1;
            acc += i % 7;
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
        check("checksum", acc, 109608L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
