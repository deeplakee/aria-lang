// word_count 参照端口(与 word_count.aria 同算法同规模)。
class Bench_word_count {
    static long[] pass() {
        String[] vocab = {"alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"};
        int words = 3000000;
        long state = 1;
        String[] parts = new String[words];
        for (int i = 0; i < words; i++) {
            state = (state * 48271) % 2147483647L;
            parts[i] = vocab[(int) (state % 8)];
        }
        String text = String.join(" ", parts);
        java.util.HashMap<String, Long> counts = new java.util.HashMap<>();
        for (String w : text.split(" ")) {
            counts.merge(w, 1L, Long::sum);
        }
        long total = 0;
        for (String w : vocab) {
            total += counts.get(w);
        }
        return new long[] {total, counts.get("alpha"), counts.get("theta")};
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
        check("checksum", v[0], 3000000L);
        check("alpha", v[1], 374756L);
        check("theta", v[2], 374622L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value alpha: " + v[1]);
        System.out.println("value theta: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
