// list_ops 参照端口(与 list_ops.aria 同算法同规模):用原生 long 数组,避免装箱。
class Bench_list_ops {
    static long pass() {
        int size = 200000;
        long[] xs = new long[size];
        for (int i = 0; i < size; i++) {
            xs[i] = i;
        }
        long acc = 0;
        for (int r = 0; r < 20; r++) {
            for (int j = 0; j < size; j++) {
                acc = (acc + xs[j] + xs[size - 1 - j]) % 1000003L;
                xs[j] = (xs[j] + 1) % 1000003L;
            }
        }
        return (acc + xs.length) % 1000003L;
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
        check("checksum", acc, 799787L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
