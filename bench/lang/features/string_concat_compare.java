// string_concat_compare 参照端口(与 string_concat_compare.aria 同算法同规模):拼接用非 final 局部
// 避免 javac 常量折叠;Java 无字符串比较算子,用本语言的 compareTo(ASCII 上与字节序同结果)。
class Bench_string_concat_compare {
    static long pass() {
        long acc = 0;
        for (long i = 0; i < 2000000L; i++) {
            String head = "abc";
            String s = head + "def" + "ghi";
            if (s.equals("abcdefghi")) {
                acc = acc + 1;
            }
            if (s.compareTo("abz") < 0) {
                acc = acc + 2;
            }
            if (s.compareTo("abcdefgh") >= 0) {
                acc = acc + 3;
            }
            acc = (acc + (s + "!").length()) % 1000003L;
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
        check("checksum", acc, 999907L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
