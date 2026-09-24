// exception_deep_unwind 参照端口(与 exception_deep_unwind.aria 同算法同规模):Java 的异常必须是
// Throwable,故用一个携带数值的自定义异常(每次 throw 都建对象,与 aria throw 原值机制不同)。
class Bench_exception_deep_unwind {
    static class BenchError extends RuntimeException {
        final long v;

        BenchError(long v) {
            super("bench");
            this.v = v;
        }
    }

    static void level3(long v) {
        throw new BenchError(v);
    }

    static void level2(long v) {
        level3(v);
    }

    static void level1(long v) {
        level2(v);
    }

    static long pass() {
        long acc = 0;
        for (long i = 0; i < 2600000L; i++) {
            try {
                level1(i);
            } catch (BenchError e) {
                acc = (acc + e.v) % 1000003L;
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
        check("checksum", acc, 560036L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
