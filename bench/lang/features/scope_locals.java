// scope_locals 参照端口(与 scope_locals.aria 同算法同规模)。
class Bench_scope_locals {
    static long work(long a) {
        long b = a + 1;
        long c = b * 2;
        long d = c - 3;
        long out = d;
        {
            long e = d + 4;
            out = e % 1000003L;
        }
        return out;
    }

    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 4500000L) {
            acc = (acc + work(i)) % 1000003L;
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
        check("checksum", acc, 250156L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
