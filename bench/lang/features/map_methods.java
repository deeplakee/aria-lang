// map_methods 参照端口(与 map_methods.aria 同算法同规模):快照序 unspecified,只做与序无关的
// 求和与计数。
class Bench_map_methods {
    static long pass() {
        int entries = 20000;
        java.util.HashMap<Long, Long> m = new java.util.HashMap<>();
        for (long i = 0; i < entries; i++) {
            m.put(i, i * 2);
        }
        long acc = 0;
        for (long r = 0; r < 1600L; r++) {
            long key = r % entries;
            m.remove(key);
            m.put(key, r);
            if (m.containsKey(key)) {
                acc = (acc + m.get(key)) % 1000003L;
            }
            acc = (acc + m.size()) % 1000003L;
            if (r % 10 == 0) {
                java.util.ArrayList<Long> ks = new java.util.ArrayList<>(m.keySet());
                java.util.ArrayList<Long> vs = new java.util.ArrayList<>(m.values());
                java.util.ArrayList<java.util.Map.Entry<Long, Long>> ps =
                        new java.util.ArrayList<>(m.entrySet());
                acc = (acc + ks.size() + vs.size() + ps.size()) % 1000003L;
                for (int j = 0; j < ks.size(); j++) {
                    acc = (acc + ks.get(j)) % 1000003L;
                }
            }
        }
        m.clear();
        return (acc + m.size()) % 1000003L;
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
        check("checksum", acc, 183077L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
