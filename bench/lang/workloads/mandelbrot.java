// mandelbrot 参照端口(与 mandelbrot.aria 同算法同规模)。
// JVM 惯例:先跑一遍热身丢弃(触发 JIT),再计时跑第二遍--见 lang-bench-notes.md「端口纪律」。
class Bench_mandelbrot {
    static long[] pass() {
        int size = 500;
        int maxIter = 50;
        long checksum = 0;
        long inside = 0;
        for (int pyIdx = 0; pyIdx < size; pyIdx++) {
            double y0 = (pyIdx * 1.0 / size) * 2.0 - 1.0;
            for (int px = 0; px < size; px++) {
                double x0 = (px * 1.0 / size) * 2.5 - 2.0;
                double x = 0.0;
                double y = 0.0;
                int i = 0;
                while (i < maxIter) {
                    double x2 = x * x;
                    double y2 = y * y;
                    if (x2 + y2 > 4.0) {
                        break;
                    }
                    y = 2.0 * x * y + y0;
                    x = x2 - y2 + x0;
                    i += 1;
                }
                checksum = (checksum * 2 + i) % 1000000007L;
                if (i == maxIter) {
                    inside += 1;
                }
            }
        }
        return new long[] {checksum, inside, (long) size * size};
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
        check("checksum", v[0], 140394512L);
        check("inside", v[1], 79596L);
        check("pixels", v[2], 250000L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value inside: " + v[1]);
        System.out.println("value pixels: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
