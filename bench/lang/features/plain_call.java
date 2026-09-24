// plain_call 参照端口(与 plain_call.aria 同算法同规模)。
class Bench_plain_call {
    static long step(long v) {
        return v + 1;
    }

    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 6000000L) {
            acc = acc + step(i);
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
