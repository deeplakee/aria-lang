// map_ops 参照端口(与 map_ops.aria 同算法同规模):键是整数值 0..entries-1(与 aria 同键集);
// HashMap<Long,Long> 的每次查表都装箱,这是 Java 侧的真实成本。
class Bench_map_ops {
    static long pass() {
        int entries = 100000;
        java.util.HashMap<Long, Long> m = new java.util.HashMap<>();
        for (long i = 0; i < entries; i++) {
            m.put(i, i * 2);
        }
        long acc = 0;
        for (int r = 0; r < 40; r++) {
            for (long j = 0; j < entries; j++) {
                if (m.containsKey(j)) {
                    acc = (acc + m.get(j)) % 1000003L;
                }
            }
        }
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
        check("checksum", acc, 900018L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
