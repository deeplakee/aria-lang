// bank_queue 参照端口(与 bank_queue.aria 同算法同规模)。
class Bench_bank_queue {
    static final long KEY_UNIT = 1000000L;

    static class Teller {
        long busy_until;
        long served;
    }

    static long state;
    static long seq;
    static long served;
    static long wait_sum;
    static long last_time;
    static Teller[] tellers;
    static java.util.ArrayList<long[]> heap;
    static java.util.ArrayList<Long> queue;
    static double inner_ms;

    static long rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return state % n;
    }

    static void heap_push(long[] entry) {
        heap.add(entry);
        int i = heap.size() - 1;
        while (i > 0) {
            int parent = (i - 1) / 2;
            if (heap.get(parent)[0] <= heap.get(i)[0]) {
                break;
            }
            long[] tmp = heap.get(parent);
            heap.set(parent, heap.get(i));
            heap.set(i, tmp);
            i = parent;
        }
    }

    static long[] heap_pop() {
        long[] top = heap.get(0);
        long[] last = heap.remove(heap.size() - 1);
        if (heap.size() > 0) {
            heap.set(0, last);
            int i = 0;
            int n = heap.size();
            while (true) {
                int l = 2 * i + 1;
                int r = l + 1;
                int m = i;
                if (l < n && heap.get(l)[0] < heap.get(m)[0]) {
                    m = l;
                }
                if (r < n && heap.get(r)[0] < heap.get(m)[0]) {
                    m = r;
                }
                if (m == i) {
                    break;
                }
                long[] tmp = heap.get(m);
                heap.set(m, heap.get(i));
                heap.set(i, tmp);
                i = m;
            }
        }
        return top;
    }

    static void start_service(int idx, long now) {
        served += 1;
        tellers[idx].served += 1;
        long svc = 20 + rnd(60);
        tellers[idx].busy_until = now + svc;
        seq += 1;
        heap_push(new long[] {(now + svc) * KEY_UNIT + seq, 1, idx});
    }

    static void on_arrival(long key) {
        long now = key / KEY_UNIT;
        last_time = now;
        long next = now + 1 + rnd(40);
        seq += 1;
        heap_push(new long[] {next * KEY_UNIT + seq, 0, 0});
        int free = -1;
        int k = 0;
        while (k < tellers.length) {
            if (tellers[k].busy_until <= now) {
                free = k;
            }
            k += 1;
        }
        if (free >= 0) {
            start_service(free, now);
        } else {
            queue.add(now);
        }
    }

    static void on_done(long key, int idx) {
        long now = key / KEY_UNIT;
        last_time = now;
        if (queue.size() > 0) {
            long arrive = queue.remove(0);
            wait_sum += now - arrive;
            start_service(idx, now);
        }
    }

    static long[] pass() {
        int customers_n = 78000;
        state = 424242L;
        seq = 0;
        served = 0;
        wait_sum = 0;
        last_time = 0;
        tellers = new Teller[4];
        int ti = 0;
        while (ti < 4) {
            tellers[ti] = new Teller();
            ti += 1;
        }
        heap = new java.util.ArrayList<>();
        queue = new java.util.ArrayList<>();

        long t0 = System.nanoTime();
        seq += 1;
        heap_push(new long[] {(1 + rnd(40)) * KEY_UNIT + seq, 0, 0});
        while (served < customers_n) {
            long[] ev = heap_pop();
            if (ev[1] == 0) {
                on_arrival(ev[0]);
            } else {
                on_done(ev[0], (int) ev[2]);
            }
        }
        long load_sum = 0;
        int k2 = 0;
        while (k2 < tellers.length) {
            load_sum += tellers[k2].served;
            k2 += 1;
        }
        check("served", served, customers_n);
        check("load_sum", load_sum, served);
        check("wait_sum", wait_sum, 101322L);
        check("last_time", last_time, 1604597L);
        inner_ms = (System.nanoTime() - t0) / 1e6;
        return new long[] {served, wait_sum, last_time, load_sum};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long[] v = pass();
        check("warmup stable served", warm[0], v[0]);
        check("warmup stable wait_sum", warm[1], v[1]);
        check("warmup stable last_time", warm[2], v[2]);
        check("warmup stable load_sum", warm[3], v[3]);
        System.out.println("value served: " + v[0]);
        System.out.println("value wait_sum: " + v[1]);
        System.out.println("value last_time: " + v[2]);
        System.out.println("value load_sum: " + v[3]);
        System.out.println("bench-time: " + inner_ms + " ms");
    }
}
