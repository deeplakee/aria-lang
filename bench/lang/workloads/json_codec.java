// json_codec 参照端口(与 json_codec.aria 同算法同规模):生成嵌套文档并序列化,再走
// 「解析 -> 再序列化 -> 逐位比对」往返,外加剧段截断片段的合法性校验。解析器手写递归下降,
// 禁用 Jackson 一类内建库;序列化按 canonical 约定对对象键排序。值容器用 LinkedHashMap
// (键后写赢、取值序与插入序一致,输出前再排序),rnd 调用步序与 aria 逐行对齐;
// aria 的 throw 任意值在 JVM 上只能近似为 throw RuntimeException(建异常对象)。
import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;

class Bench_json_codec {
    static long state = 7;
    static double lastMs;

    static int rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return (int) (state % n);
    }

    static final String[] names = {"id", "name", "count", "active", "tags", "meta", "score", "note", "kind", "level"};
    static final String[] words = {"core", "edge", "node", "wire", "mesh", "flux", "grid", "pulse"};

    static String genString() {
        int n = 2 + rnd(4);
        String out = "";
        for (int i = 0; i < n; i++) {
            out += words[rnd(words.length)];
            int k = rnd(9);
            if (k == 0) {
                out += "\\";
            } else if (k == 1) {
                out += "\"";
            } else if (k == 2) {
                out += "\n";
            }
        }
        return out;
    }

    static Object genValue(int depth) {
        int t = rnd(10);
        if (depth >= 3 || t < 3) {
            return rnd(100000);
        }
        if (t == 3) {
            return genString();
        }
        if (t == 4) {
            return null;
        }
        if (t <= 6) {
            ArrayList<Object> arr = new ArrayList<>();
            int n = 1 + rnd(4);
            for (int i = 0; i < n; i++) {
                arr.add(genValue(depth + 1));
            }
            return arr;
        }
        LinkedHashMap<String, Object> obj = new LinkedHashMap<>();
        int n = 1 + rnd(3);
        for (int i = 0; i < n; i++) {
            // put 的实参按 JLS 自左向右求值:键的 rnd 先于值的递归,与 aria 一致。
            obj.put(names[rnd(names.length)], genValue(depth + 1));
        }
        return obj;
    }

    static Object genDoc() {
        LinkedHashMap<String, Object> obj = new LinkedHashMap<>();
        int n = 3 + rnd(4);
        for (int i = 0; i < n; i++) {
            obj.put(names[rnd(names.length)], genValue(1));
        }
        return obj;
    }

    static String escapeJson(String s) {
        String out = "\"";
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') {
                out += "\\" + c;
            } else if (c == '\n') {
                out += "\\n";
            } else if (c == '\t') {
                out += "\\t";
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

    static String jsonStringify(Object v) {
        if (v == null) {
            return "null";
        }
        if (v instanceof Integer) {
            return v.toString();
        }
        if (v instanceof String) {
            return escapeJson((String) v);
        }
        if (v instanceof ArrayList) {
            String out = "[";
            boolean first = true;
            for (Object item : (ArrayList<?>) v) {
                if (!first) {
                    out += ",";
                }
                first = false;
                out += jsonStringify(item);
            }
            return out + "]";
        }
        // 通配符捕获型,避免 unchecked 警告(javac 在中文 Windows 上会以 GBK 打提示,干扰驱动读取)。
        LinkedHashMap<?, ?> obj = (LinkedHashMap<?, ?>) v;
        ArrayList<String> ks = new ArrayList<>();
        for (Object k : obj.keySet()) {
            ks.add((String) k);
        }
        Collections.sort(ks);
        String out = "{";
        boolean first = true;
        for (String k : ks) {
            if (!first) {
                out += ",";
            }
            first = false;
            out += escapeJson(k) + ":" + jsonStringify(obj.get(k));
        }
        return out + "}";
    }

    static final class JsonParser {
        final String src;
        int pos;

        JsonParser(String src) {
            this.src = src;
            this.pos = 0;
        }

        RuntimeException fail(String msg) {
            return new RuntimeException("json: " + msg);
        }

        void skipWs() {
            while (pos < src.length()) {
                char c = src.charAt(pos);
                if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
                    pos += 1;
                } else {
                    break;
                }
            }
        }

        char peek() {
            if (pos >= src.length()) {
                throw fail("unexpected end");
            }
            return src.charAt(pos);
        }

        void expect(char ch) {
            if (peek() != ch) {
                throw fail("expected '" + ch + "' at " + pos);
            }
            pos += 1;
        }

        Object parseLit(String word, Object value) {
            int end = pos + word.length();
            if (end > src.length() || !src.substring(pos, end).equals(word)) {
                throw fail("bad literal");
            }
            pos = end;
            return value;
        }

        int parseNumber() {
            int start = pos;
            while (pos < src.length()) {
                char c = src.charAt(pos);
                if (c == '-' || (c >= '0' && c <= '9')) {
                    pos += 1;
                } else {
                    break;
                }
            }
            if (pos == start) {
                throw fail("bad number at " + start);
            }
            try {
                return Integer.parseInt(src.substring(start, pos));
            } catch (NumberFormatException e) {
                throw fail("bad number at " + start);
            }
        }

        String parseString() {
            expect('"');
            String out = "";
            while (true) {
                if (pos >= src.length()) {
                    throw fail("unterminated string");
                }
                char c = src.charAt(pos);
                if (c == '"') {
                    pos += 1;
                    return out;
                }
                if (c == '\\') {
                    pos += 1;
                    char e = peek();
                    if (e == 'n') {
                        out += "\n";
                    } else if (e == 't') {
                        out += "\t";
                    } else {
                        out += e;
                    }
                    pos += 1;
                    continue;
                }
                out += c;
                pos += 1;
            }
        }

        ArrayList<Object> parseArray() {
            expect('[');
            ArrayList<Object> arr = new ArrayList<>();
            skipWs();
            if (peek() == ']') {
                pos += 1;
                return arr;
            }
            while (true) {
                arr.add(parseValue());
                skipWs();
                if (peek() == ',') {
                    pos += 1;
                    continue;
                }
                expect(']');
                return arr;
            }
        }

        LinkedHashMap<String, Object> parseObject() {
            expect('{');
            LinkedHashMap<String, Object> obj = new LinkedHashMap<>();
            skipWs();
            if (peek() == '}') {
                pos += 1;
                return obj;
            }
            while (true) {
                String key = parseString();
                skipWs();
                expect(':');
                obj.put(key, parseValue());
                skipWs();
                if (peek() == ',') {
                    pos += 1;
                    continue;
                }
                expect('}');
                return obj;
            }
        }

        Object parseValue() {
            skipWs();
            char c = peek();
            if (c == '{') {
                return parseObject();
            }
            if (c == '[') {
                return parseArray();
            }
            if (c == '"') {
                return parseString();
            }
            if (c == 'n') {
                return parseLit("null", null);
            }
            return parseNumber();
        }
    }

    static Object parseJson(String src) {
        JsonParser p = new JsonParser(src);
        Object v = p.parseValue();
        p.skipWs();
        if (p.pos != src.length()) {
            throw p.fail("trailing content");
        }
        return v;
    }

    static long countNodes(Object v) {
        if (v instanceof ArrayList) {
            long n = 1;
            for (Object item : (ArrayList<?>) v) {
                n += countNodes(item);
            }
            return n;
        }
        if (v instanceof LinkedHashMap) {
            long n = 1;
            LinkedHashMap<?, ?> obj = (LinkedHashMap<?, ?>) v;
            for (Object k : obj.keySet()) {
                n += countNodes(obj.get(k));
            }
            return n;
        }
        return 1;
    }

    static long[] pass() {
        state = 7;
        long t0 = System.nanoTime();
        int docCount = 120;  // 规模:文档数
        int passes = 20;     // 规模:往返遍数
        int snippets = 2000; // 规模:校验片段数
        ArrayList<String> docs = new ArrayList<>();
        for (int i = 0; i < docCount; i++) {
            docs.add(jsonStringify(genDoc()));
        }
        long nodeAcc = 0;
        long roundtripOk = 0;
        for (int p = 0; p < passes; p++) {
            for (String text : docs) {
                Object v = parseJson(text);
                if (jsonStringify(v).equals(text)) {
                    roundtripOk += 1;
                }
                nodeAcc = (nodeAcc + countNodes(v)) % 1000003L;
            }
        }
        long valid = 0;
        long invalid = 0;
        for (int i = 0; i < snippets; i++) {
            String text = docs.get(i % docCount);
            String probe = text.substring(0, 1 + rnd(text.length() / 2));
            if (rnd(3) == 0) {
                probe = probe + "}";
            } else if (rnd(3) == 0) {
                probe = probe + text.charAt(0);
            }
            try {
                parseJson(probe);
                valid += 1;
            } catch (RuntimeException e) {
                invalid += 1;
            }
        }
        lastMs = (System.nanoTime() - t0) / 1e6;
        return new long[] {roundtripOk, nodeAcc, valid, invalid};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("value drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long[] v = pass();
        check("warmup stable roundtrip_ok", warm[0], v[0]);
        check("warmup stable node_acc", warm[1], v[1]);
        check("warmup stable valid", warm[2], v[2]);
        check("warmup stable invalid", warm[3], v[3]);
        check("roundtrip_ok", v[0], 2400L);
        check("node_acc", v[1], 31480L);
        check("valid", v[2], 58L);
        check("invalid", v[3], 1942L);
        System.out.println("value roundtrip_ok: " + v[0]);
        System.out.println("value node_acc: " + v[1]);
        System.out.println("value valid: " + v[2]);
        System.out.println("value invalid: " + v[3]);
        System.out.println("bench-time: " + lastMs + " ms");
    }
}
