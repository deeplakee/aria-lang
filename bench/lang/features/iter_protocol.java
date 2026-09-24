// iter_protocol 参照端口(与 iter_protocol.aria 同算法同规模):四种源按本语言的迭代写法遍历。
class Bench_iter_protocol {
    static long pass() {
        int size = 512;
        long[] xs = new long[size];
        for (int i = 0; i < size; i++) {
            xs[i] = i;
        }
        java.util.HashMap<Long, Long> m = new java.util.HashMap<>();
        for (long k = 0; k < size; k++) {
            m.put(k, k);
        }
        long acc = 0;
        for (int r = 0; r < 4000; r++) {
            for (long v : xs) {
                acc = (acc + v) % 1000003L;
            }
            for (long v = 0; v < size; v++) {
                acc = (acc + v) % 1000003L;
            }
            for (char ch : "abc".toCharArray()) {
                acc = acc + 1;
            }
            for (Long k : m.keySet()) {
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
        check("checksum", acc, 799293L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
