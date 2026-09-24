// closure_upvalue 参照端口(与 closure_upvalue.aria 同算法同规模):Java 的 lambda 只能捕获
// effectively-final 的局部,可变捕获用本语言的惯用写法(单元素数组当持有槽)。
class Bench_closure_upvalue {
    static java.util.function.LongSupplier makeCounter() {
        long[] n = {0};
        return () -> {
            n[0] = n[0] + 1;
            return n[0];
        };
    }

    static long pass() {
        java.util.function.LongSupplier next = makeCounter();
        long acc = 0;
        long i = 0;
        while (i < 6000000L) {
            acc = acc + next.getAsLong();
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
        check("checksum", acc, 18000003000000L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
