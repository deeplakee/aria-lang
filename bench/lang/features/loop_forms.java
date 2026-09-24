// loop_forms 参照端口(与 loop_forms.aria 同算法同规模)。
class Bench_loop_forms {
    static long pass() {
        long acc = 0;
        for (long r = 0; r < 500000L; r++) {
            for (long k = 0; k < 16; k += 1) {
                if (k % 3 == 0) {
                    continue;
                }
                if (k > 12) {
                    break;
                }
                acc = (acc + k) % 1000003L;
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
        check("checksum", acc, 999931L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
