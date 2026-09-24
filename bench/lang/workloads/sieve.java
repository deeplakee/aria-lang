// sieve 参照端口(与 sieve.aria 同算法同规模):字节容器用本语言自然的 boolean[]。
class Bench_sieve {
    static long[] pass() {
        int[] sizes = {100000, 1000000, 4000000};
        long[] counts = new long[sizes.length];
        long total = 0;
        for (int s = 0; s < sizes.length; s++) {
            int n = sizes[s];
            boolean[] composite = new boolean[n];
            long count = 0;
            for (int i = 2; i < n; i++) {
                if (!composite[i]) {
                    count += 1;
                    for (long j = (long) i * i; j < n; j += i) {
                        composite[(int) j] = true;
                    }
                }
            }
            counts[s] = count;
            total += count;
        }
        return new long[] {counts[0], counts[1], counts[2], total};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm[0], v[0]);
        check("count_100000", v[0], 9592L);
        check("count_1000000", v[1], 78498L);
        check("count_4000000", v[2], 283146L);
        check("checksum", v[3], 371236L);
        System.out.println("value count_100000: " + v[0]);
        System.out.println("value count_1000000: " + v[1]);
        System.out.println("value count_4000000: " + v[2]);
        System.out.println("value checksum: " + v[3]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
