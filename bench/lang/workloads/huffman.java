// huffman 参照端口(与 huffman.aria 同算法同规模)。建树队列按词首次出现序(不依赖 map 迭代序)。
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

class Bench_huffman {
    static long state;

    static int rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return (int) (state % n);
    }

    static String genChar(String letters, int[] weights) {
        int r = rnd(1000);
        int acc = 0;
        for (int i = 0; i < weights.length; i++) {
            acc += weights[i];
            if (r < acc) {
                return String.valueOf(letters.charAt(i));
            }
        }
        return String.valueOf(letters.charAt(0));
    }

    static void walk(HNode node, String prefix, Map<Character, String> codes) {
        if (node.left == null) {
            codes.put(node.ch.charAt(0), prefix);
            return;
        }
        walk(node.left, prefix + "0", codes);
        walk(node.right, prefix + "1", codes);
    }

    static long[] pass() {
        int messages = 1150;
        int msgLen = 80;
        String letters = "etaoinshrdlucmfwypvbgkxjqz";
        int[] weights = {113, 90, 80, 75, 70, 67, 63, 61, 60, 43, 40, 28, 24, 22, 21, 19, 19, 18, 17, 16, 15, 13, 10, 9, 5, 2};
        state = 31337;

        List<String> texts = new ArrayList<>();
        for (int mi = 0; mi < messages; mi++) {
            StringBuilder parts = new StringBuilder();
            for (int j = 0; j < msgLen; j++) {
                parts.append(genChar(letters, weights));
            }
            texts.add(parts.toString());
        }

        Map<Character, Integer> freq = new HashMap<>();
        List<Character> order = new ArrayList<>();
        for (String msg : texts) {
            for (int i = 0; i < msg.length(); i++) {
                char ch = msg.charAt(i);
                if (freq.containsKey(ch)) {
                    freq.put(ch, freq.get(ch) + 1);
                } else {
                    freq.put(ch, 1);
                    order.add(ch);
                }
            }
        }

        List<HNode> pq = new ArrayList<>();
        for (char ch : order) {
            int n = freq.get(ch);
            int pos = pq.size();
            while (pos > 0 && pq.get(pos - 1).w > n) {
                pos -= 1;
            }
            pq.add(pos, new HNode(n, String.valueOf(ch), null, null));
        }
        while (pq.size() > 1) {
            HNode a = pq.remove(0);
            HNode b = pq.remove(0);
            HNode merged = new HNode(a.w + b.w, "", a, b);
            int pos = pq.size();
            while (pos > 0 && pq.get(pos - 1).w > merged.w) {
                pos -= 1;
            }
            pq.add(pos, merged);
        }
        HNode root = pq.get(0);

        Map<Character, String> codes = new HashMap<>();
        walk(root, "", codes);

        int bitTotal = 0;
        List<String> encoded = new ArrayList<>();
        for (String msg : texts) {
            StringBuilder bits = new StringBuilder();
            for (int i = 0; i < msg.length(); i++) {
                String code = codes.get(msg.charAt(i));
                bitTotal += code.length();
                for (int j = 0; j < code.length(); j++) {
                    bits.append(code.charAt(j));
                }
            }
            encoded.add(bits.toString());
        }

        int decodedOk = 0;
        for (int di = 0; di < messages; di++) {
            String enc = encoded.get(di);
            HNode node = root;
            StringBuilder out = new StringBuilder();
            for (int i = 0; i < enc.length(); i++) {
                if (enc.charAt(i) == '0') {
                    node = node.left;
                } else {
                    node = node.right;
                }
                if (node.left == null) {
                    out.append(node.ch);
                    node = root;
                }
            }
            if (out.toString().equals(texts.get(di))) {
                decodedOk += 1;
            }
        }

        return new long[] {codes.size(), bitTotal, decodedOk};
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm[1], v[1]);
        check("symbols", v[0], 26L);
        check("bit_total", v[1], 395969L);
        check("decoded_ok", v[2], 1150L);
        System.out.println("value symbols: " + v[0]);
        System.out.println("value bit_total: " + v[1]);
        System.out.println("value decoded_ok: " + v[2]);
        System.out.println("bench-time: " + ms + " ms");
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }
}

class HNode {
    int w;
    String ch;
    HNode left;
    HNode right;

    HNode(int w, String ch, HNode left, HNode right) {
        this.w = w;
        this.ch = ch;
        this.left = left;
        this.right = right;
    }
}
