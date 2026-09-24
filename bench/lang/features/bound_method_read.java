// bound_method_read 参照端口(与 bound_method_read.aria 同算法同规模):Java 用方法引用(obj::step),
// 每轮建一个新的绑定方法对象,与 aria 读路径现场绑定同性质。
class Bench_bound_method_read {
    static class Cell {
        long v;

        Cell(long v) {
            this.v = v;
        }

        long step(long d) {
            return this.v + d;
        }
    }

    static long pass() {
        Cell obj = new Cell(1);
        long acc = 0;
        long i = 0;
        while (i < 3800000L) {
            java.util.function.LongUnaryOperator f = obj::step;
            acc = (acc + f.applyAsLong(i)) % 1000003L;
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
        check("checksum", acc, 240060L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
