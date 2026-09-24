// method_dispatch 参照端口(与 method_dispatch.aria 同算法同规模)。
class Bench_method_dispatch {
    static class Counter {
        long base = 1;

        long step(long delta) {
            return this.base + delta;
        }
    }

    static long pass() {
        Counter counter = new Counter();
        long acc = 0;
        long i = 0;
        while (i < 5000000L) {
            acc = (acc + counter.step(i)) % 1000003L;
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
        check("checksum", acc, 105L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
