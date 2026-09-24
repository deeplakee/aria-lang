// list_slice 参照端口(与 list_slice.aria 同算法同规模):aria 的区间切片两端皆含,故 Java 侧上界 +1。
class Bench_list_slice {
    static long pass() {
        int size = 512;
        long[] xs = new long[size];
        for (int i = 0; i < size; i++) {
            xs[i] = i;
        }
        long acc = 0;
        for (int r = 0; r < 900000; r++) {
            long[] a = java.util.Arrays.copyOfRange(xs, 10, 21);
            long[] b = java.util.Arrays.copyOfRange(xs, size - 20, size);
            long[] c = new long[11];
            for (int i = 0; i < 11; i++) {
                c[i] = xs[20 - i];
            }
            acc = (acc + a.length + b.length + c.length + a[0] + b[0] + c[0]) % 1000003L;
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
        check("checksum", acc, 598479L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
