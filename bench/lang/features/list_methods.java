// list_methods 参照端口(与 list_methods.aria 同算法同规模):ArrayList 的方法面(元素装箱)。
class Bench_list_methods {
    static String join(java.util.ArrayList<Long> xs) {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < xs.size(); i++) {
            if (i > 0) {
                sb.append(',');
            }
            sb.append(xs.get(i));
        }
        return sb.toString();
    }

    static long pass() {
        int size = 2000;
        java.util.ArrayList<Long> xs = new java.util.ArrayList<>();
        for (long i = 0; i < size; i++) {
            xs.add(i);
        }
        long acc = 0;
        for (long r = 0; r < 800000L; r++) {
            xs.add(r);
            xs.add(0, r);
            if (xs.contains(r)) {
                acc = (acc + xs.indexOf(r)) % 1000003L;
            }
            acc = (acc + xs.remove(2)) % 1000003L;
            acc = (acc + xs.remove(xs.size() - 1)) % 1000003L;
            acc = (acc + xs.size()) % 1000003L;
        }
        acc = (acc + join(xs).length()) % 1000003L;
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
        check("checksum", acc, 684118L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
