// markov 参照端口(与 markov.aria 同算法同规模)。
// 状态表的行是 [词, 计数, ...] 交织数组,Java 侧拆成词/计数两条平行列表(线性扫的负载形态不变)。
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;

class Bench_markov {
    static long state;

    static int rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return (int) (state % n);
    }

    static long[] pass() {
        int sentences = 6500;
        int genWords = 20000;
        String[] vocab = {"the", "of", "and", "a", "to", "in", "is", "it", "was", "that", "for", "time",
                          "world", "life", "mind", "light", "river", "stone", "forest", "winter", "summer",
                          "harbor", "signal", "memory", "shadow", "window", "garden", "mirror", "voyage", "silence"};
        state = 12345;

        List<String> corpus = new ArrayList<>();
        for (int i = 0; i < sentences; i++) {
            int len = 6 + rnd(9);
            for (int j = 0; j < len; j++) {
                corpus.add(vocab[rnd(vocab.length)]);
            }
        }

        Map<String, MarkovRow> states = new HashMap<>();
        String w0 = corpus.get(0);
        String w1 = corpus.get(1);
        for (int i = 2; i < corpus.size(); i++) {
            String key = w0 + " " + w1;
            MarkovRow row = states.get(key);
            if (row == null) {
                row = new MarkovRow();
                states.put(key, row);
            }
            String w2 = corpus.get(i);
            boolean found = false;
            int p = 0;
            while (p < row.words.size()) {
                if (row.words.get(p).equals(w2)) {
                    row.counts.set(p, row.counts.get(p) + 1);
                    found = true;
                }
                p += 1;
            }
            if (!found) {
                row.words.add(w2);
                row.counts.add(1);
            }
            w0 = w1;
            w1 = w2;
        }

        List<String> produced = new ArrayList<>();
        long checksum = 0;
        String g0 = corpus.get(0);
        String g1 = corpus.get(1);
        int made = 0;
        while (made < genWords) {
            MarkovRow row = states.get(g0 + " " + g1);
            if (row == null) {
                g0 = corpus.get(0);
                g1 = corpus.get(1);
                continue;
            }
            int total = 0;
            for (int p = 0; p < row.counts.size(); p++) {
                total += row.counts.get(p);
            }
            int pick = rnd(total);
            String w = row.words.get(0);
            for (int p = 0; p < row.counts.size(); p++) {
                if (pick < row.counts.get(p)) {
                    w = row.words.get(p);
                    break;
                }
                pick -= row.counts.get(p);
            }
            produced.add(w);
            checksum = (checksum * 31 + w.length()) % 1000003L;
            g0 = g1;
            g1 = w;
            made += 1;
        }

        HashSet<String> seen = new HashSet<>(produced);

        return new long[] {states.size(), checksum, seen.size()};
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm[1], v[1]);
        check("states", v[0], 900L);
        check("checksum", v[1], 679528L);
        check("distinct", v[2], 30L);
        System.out.println("value states: " + v[0]);
        System.out.println("value checksum: " + v[1]);
        System.out.println("value distinct: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }
}

class MarkovRow {
    ArrayList<String> words = new ArrayList<>();
    ArrayList<Integer> counts = new ArrayList<>();
}
