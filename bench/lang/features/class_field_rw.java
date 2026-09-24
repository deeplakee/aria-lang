// class_field_rw 参照端口(与 class_field_rw.aria 同算法同规模)。
class Bench_class_field_rw {
    static class Cell {
        long v;
    }

    static long pass() {
        Cell cell = new Cell();
        long i = 0;
        while (i < 7000000L) {
            cell.v = cell.v + 1;
            i += 1;
        }
        return cell.v;
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
        check("checksum", acc, 7000000L);
        System.out.println("value checksum: " + acc);
        System.out.println("bench-time: " + ms + " ms");
    }
}
