// lambda_higher_order 参照端口(与 lambda_higher_order.aria 同算法同规模):用 LongUnaryOperator
// 避免装箱。
class Bench_lambda_higher_order {
    static java.util.function.LongUnaryOperator compose(java.util.function.LongUnaryOperator f,
                                                       java.util.function.LongUnaryOperator g) {
        return x -> f.applyAsLong(g.applyAsLong(x));
    }

    static long pass() {
        java.util.function.LongUnaryOperator chain =
                compose(x -> x + 1, x -> x * 2);
        long acc = 0;
        long i = 0;
        while (i < 5000000L) {
            acc = chain.applyAsLong(acc) % 1000003L;
            i += 1;
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
        check("checksum", acc, 395508L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
