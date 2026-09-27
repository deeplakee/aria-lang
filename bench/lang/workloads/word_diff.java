// word_diff 参照端口(与 word_diff.aria 同算法同规模):LCG 造基准文档,按比例做替换/插词/删词得
// 修订版,再对每对版本跑 LCS 全表动态规划 + 回溯,统计保留/删除/插入词数(评审工具的词级 diff)。
import java.util.ArrayList;
import java.util.List;

class Bench_word_diff {
    static final int DOC_SIZE = 130;  // 规模:文档词数
    static final int PAIRS = 42;      // 规模:版本对数
    static final String[] VOCAB = {
        "alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa",
        "lambda", "mu",
    };

    static int state = 555;

    static int rnd(int n) {
        state = (int) ((state * 48271L) % 2147483647L);
        return state % n;
    }

    static List<String> genDoc() {
        List<String> d = new ArrayList<>();
        for (int i = 0; i < DOC_SIZE; i++) {
            d.add(VOCAB[rnd(VOCAB.length)]);
        }
        return d;
    }

    static List<String> mutate(List<String> src) {
        List<String> out = new ArrayList<>();
        for (int i = 0; i < src.size(); i++) {
            int roll = rnd(20);
            if (roll == 0) {
                // 删词:跳过
            } else if (roll == 1) {
                out.add(VOCAB[rnd(VOCAB.length)]);
            } else if (roll == 2) {
                out.add(src.get(i));
                out.add(VOCAB[rnd(VOCAB.length)]);
            } else {
                out.add(src.get(i));
            }
        }
        return out;
    }

    static long[] diffCounts(List<String> a, List<String> b) {
        int n1 = a.size();
        int n2 = b.size();
        int[][] t = new int[n1 + 1][n2 + 1];
        for (int i = 1; i <= n1; i++) {
            for (int j = 1; j <= n2; j++) {
                if (a.get(i - 1).equals(b.get(j - 1))) {
                    t[i][j] = t[i - 1][j - 1] + 1;
                } else {
                    int up = t[i - 1][j];
                    int left = t[i][j - 1];
                    t[i][j] = up >= left ? up : left;
                }
            }
        }
        long eq = 0;
        long del = 0;
        long ins = 0;
        int i = n1;
        int j = n2;
        while (i > 0 && j > 0) {
            if (a.get(i - 1).equals(b.get(j - 1))) {
                eq += 1;
                i -= 1;
                j -= 1;
            } else if (t[i - 1][j] >= t[i][j - 1]) {
                del += 1;
                i -= 1;
            } else {
                ins += 1;
                j -= 1;
            }
        }
        del += i;
        ins += j;
        return new long[] {eq, del, ins};
    }

    static long[] pass() {
        state = 555;  // 每轮从同一初始态起跑,保证可重复
        long eqSum = 0;
        long delSum = 0;
        long insSum = 0;
        for (int p = 0; p < PAIRS; p++) {
            List<String> a = genDoc();
            List<String> b = mutate(a);
            long[] c = diffCounts(a, b);
            eqSum = (eqSum + c[0]) % 1000003L;
            delSum = (delSum + c[1]) % 1000003L;
            insSum = (insSum + c[2]) % 1000003L;
        }
        return new long[] {eqSum, delSum, insSum};
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
        check("warmup stable del", warm[1], v[1]);
        check("warmup stable ins", warm[2], v[2]);
        check("eq_sum", v[0], 4966L);
        check("del_sum", v[1], 494L);
        check("ins_sum", v[2], 503L);
        System.out.println("value eq_sum: " + v[0]);
        System.out.println("value del_sum: " + v[1]);
        System.out.println("value ins_sum: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
