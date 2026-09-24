// match_dispatch 参照端口(与 match_dispatch.aria 同算法同规模):Java 用本语言的多路派发 switch。
class Bench_match_dispatch {
    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 4000000L) {
            long bucket;
            switch ((int) (i % 5)) {
                case 0: bucket = 3; break;
                case 1: bucket = 5; break;
                case 2: bucket = 7; break;
                case 3: bucket = 11; break;
                default: bucket = 13; break;
            }
            acc = (acc + bucket) % 1000003L;
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
        check("checksum", acc, 199907L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
