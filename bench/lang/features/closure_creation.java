// closure_creation 参照端口(与 closure_creation.aria 同算法同规模):每轮建一个捕获参数的 lambda。
class Bench_closure_creation {
    static java.util.function.LongUnaryOperator make(long base) {
        return d -> base + d;
    }

    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 2800000L) {
            java.util.function.LongUnaryOperator f = make(i);
            acc = (acc + f.applyAsLong(2)) % 1000003L;
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
        check("checksum", acc, 440024L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
