// logic_short_circuit 参照端口(与 logic_short_circuit.aria 同算法同规模)。
class Bench_logic_short_circuit {
    static long pass() {
        long acc = 0;
        long i = 0;
        while (i < 6000000L) {
            if ((i > 0 && i % 2 == 0) || i % 7 == 0) {
                acc = acc + 1;
            }
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
        check("checksum", acc, 3428571L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
