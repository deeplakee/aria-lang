// string_codepoint 参照端口(与 string_codepoint.aria 同算法同规模):Java 用 codePointCount /
// codePointAt 取码点域读数;字节域口径各语言不同,不进校验和。
class Bench_string_codepoint {
    static long pass() {
        String base = "héllo wörld";
        long acc = 0;
        for (long i = 0; i < 1100000L; i++) {
            int count = base.codePointCount(0, base.length());
            acc = (acc + count + base.codePointAt(1) + base.codePointAt(4) +
                   base.codePointAt(0)) % 1000003L;
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
        String base = "héllo wörld";
        long warm = pass();
        long t0 = System.nanoTime();
        long acc = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm, acc);
        check("checksum", acc, 898488L);
        check("codepoints", base.codePointCount(0, base.length()), 11L);
        System.out.println("value checksum: " + acc);
        System.out.println("value codepoints: " + base.codePointCount(0, base.length()));
        System.out.println("bench-time: " + ms + " ms");
    }
}
