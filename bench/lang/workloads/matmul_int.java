// matmul_int 参照端口(与 matmul_int.aria 同算法同规模)。
class Bench_matmul_int {
    static long pass() {
        int n = 250;
        long[][] a = new long[n][n];
        long[][] b = new long[n][n];
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                a[i][j] = (i * 7 + j * 13 + 1) % 1009;
                b[i][j] = (i * 11 + j * 3 + 2) % 1009;
            }
        }
        long checksum = 0;
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < n; j++) {
                long acc = 0;
                for (int k = 0; k < n; k++) {
                    acc += a[i][k] * b[k][j];
                }
                checksum = (checksum * 31 + acc) % 1000000007L;
            }
        }
        return checksum;
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
        check("checksum", acc, 396310743L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
