// class_static_member 参照端口(与 class_static_member.aria 同算法同规模):static 字段即静态成员。
class Bench_class_static_member {
    static class Counter {
        static long n = 0;

        static long add(long d) {
            Counter.n = Counter.n + d;
            return Counter.n;
        }
    }

    static long pass() {
        Counter.n = 0;
        long acc = 0;
        long i = 0;
        while (i < 4200000L) {
            acc = (acc + Counter.add(1)) % 1000003L;
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
        check("checksum", acc, 640075L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
