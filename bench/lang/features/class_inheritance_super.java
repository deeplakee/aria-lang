// class_inheritance_super 参照端口(与 class_inheritance_super.aria 同算法同规模)。
class Bench_class_inheritance_super {
    static class Base {
        long v;

        Base(long v) {
            this.v = v;
        }

        long describe() {
            return 1;
        }
    }

    static class Derived extends Base {
        Derived(long v) {
            super(v);
        }

        long describe() {
            return 10 + super.describe();
        }
    }

    static long pass() {
        Derived obj = new Derived(7);
        long acc = 0;
        long i = 0;
        while (i < 4000000L) {
            acc = (acc + obj.describe()) % 1000003L;
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
        check("checksum", acc, 999871L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
