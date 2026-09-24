// string_ops 参照端口(与 string_ops.aria 同算法同规模)。
class Bench_string_ops {
    static long pass() {
        String base = "the quick brown fox jumps over the lazy dog";
        long acc = 0;
        for (long i = 0; i < 1000000L; i++) {
            if (base.contains("brown")) {
                acc += base.indexOf("fox");
            }
            String[] parts = base.split(" ");
            acc = (acc + parts.length + base.substring(4, 9).length()) % 1000003L;
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
        check("checksum", acc, 999913L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
