// fasta 参照端口(与 fasta.aria 同算法同规模):不含经典版的行宽折行。
class Bench_fasta {
    static long[] pass() {
        long im = 139968;
        long ia = 3877;
        long ic = 29573;
        long state = 42;
        char[] chars = {'a', 'c', 'g', 't', 'B', 'D', 'H', 'K', 'M', 'N', 'R', 'S', 'V', 'W', 'Y'};
        double[] probs = {0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02};
        double[] cum = new double[probs.length];
        double running = 0.0;
        for (int i = 0; i < probs.length; i++) {
            running = running + probs[i];
            cum[i] = running;
        }
        int bases = 2000000;
        char[] buf = new char[bases];
        long checksum = 0;
        for (int i = 0; i < bases; i++) {
            state = (state * ia + ic) % im;
            double pick = state * 1.0 / im;
            int idx = 0;
            while (idx < 15 && cum[idx] <= pick) {
                idx += 1;
            }
            if (idx == 15) {
                idx = 14;
            }
            buf[i] = chars[idx];
            checksum = (checksum * 131 + chars[idx]) % 1000000007L;
        }
        String seq = new String(buf);
        return new long[] {checksum, seq.length()};
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
        check("checksum", v[0], 575313243L);
        check("length", v[1], 2000000L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value length: " + v[1]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
