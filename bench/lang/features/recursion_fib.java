// recursion_fib 参照端口(与 recursion_fib.aria 同算法同规模)。
class Bench_recursion_fib {
    static long fib(long n) {
        if (n < 2) {
            return n;
        }
        return fib(n - 1) + fib(n - 2);
    }

    static long pass() {
        long acc = 0;
        for (long r = 0; r < 20; r++) {
            acc += fib(27);
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
        check("checksum", acc, 3928360L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
