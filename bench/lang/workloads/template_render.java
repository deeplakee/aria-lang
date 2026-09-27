// template_render 参照端口(与 template_render.aria 同算法同规模):mustache 式模板编译成捕获
// 片段的闭包列表(文本/变量/section 三种段,section 递归编译),对 LCG 造的订单数据反复渲染;
// 变量段做 HTML 转义与缺失键兜底。段闭包用 Function<Map<String, Object>, String> 的 lambda 表达。
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.function.Function;

class Bench_template_render {
    static long state = 2024;

    static long rnd(int n) {
        state = (state * 48271L) % 2147483647L;
        return state % n;
    }

    static String esc_html(String s) {
        return s.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;");
    }

    static Object lookup(Map<String, Object> d, String name) {
        if (d.containsKey(name)) {
            return d.get(name);
        }
        return null;
    }

    static int find_from(String src, String needle, int from) {
        // indexOf 带 fromIndex 即「在 src.substring(from) 里找」,命中返回 from + 局部偏移,未命中 -1。
        return src.indexOf(needle, from);
    }

    static String render(List<Function<Map<String, Object>, String>> segs, Map<String, Object> data) {
        StringBuilder out = new StringBuilder();
        for (Function<Map<String, Object>, String> seg : segs) {
            out.append(seg.apply(data));
        }
        return out.toString();
    }

    // 段工厂:lambda 捕获 effectively-final 参数,即当轮值(等价 aria 的逐段捕获)。
    static Function<Map<String, Object>, String> text_seg(String t) {
        return d -> t;
    }

    static Function<Map<String, Object>, String> var_seg(String name) {
        return d -> {
            Object v = lookup(d, name);
            if (v == null) {
                return "";
            }
            return esc_html(String.valueOf(v));
        };
    }

    @SuppressWarnings("unchecked")
    static List<Map<String, Object>> as_items(Object v) {
        return (List<Map<String, Object>>) v;
    }

    static Function<Map<String, Object>, String> section_seg(String name, List<Function<Map<String, Object>, String>> body) {
        return d -> {
            Object items = lookup(d, name);
            if (items == null) {
                return "";
            }
            StringBuilder out = new StringBuilder();
            for (Map<String, Object> item : as_items(items)) {
                out.append(render(body, item));
            }
            return out.toString();
        };
    }

    static List<Function<Map<String, Object>, String>> compile(String src) {
        List<Function<Map<String, Object>, String>> segs = new ArrayList<>();
        int pos = 0;
        while (true) {
            int open = find_from(src, "{{", pos);
            if (open < 0) {
                String rest = src.substring(pos);
                if (rest.length() > 0) {
                    segs.add(text_seg(rest));
                }
                break;
            }
            if (open > pos) {
                String lit = src.substring(pos, open);
                segs.add(text_seg(lit));
            }
            int close = find_from(src, "}}", open);
            String tag = src.substring(open + 2, close);
            if (tag.charAt(0) == '#') {
                String name = tag.substring(1);
                String endtag = "{{/" + name + "}}";
                int stop = find_from(src, endtag, close);
                List<Function<Map<String, Object>, String>> body = compile(src.substring(close + 2, stop));
                segs.add(section_seg(name, body));
                pos = stop + endtag.length();
            } else {
                segs.add(var_seg(tag));
                pos = close + 2;
            }
        }
        return segs;
    }

    static long[] pass() {
        int orders_n = 900;  // 规模:订单数
        int rounds = 20;     // 规模:渲染轮数
        state = 2024;

        String[] item_names = {"widget", "gadget", "cable", "mount", "panel", "sensor"};
        String[] buyers = {"acme", "globex", "initech", "umbrella", "stark", "wayne"};
        List<Map<String, Object>> orders = new ArrayList<>();
        for (int i = 0; i < orders_n; i++) {
            List<Map<String, Object>> items = new ArrayList<>();
            int n = 1 + (int) rnd(4);
            for (int j = 0; j < n; j++) {
                Map<String, Object> item = new HashMap<>();
                item.put("name", item_names[(int) rnd(item_names.length)]);
                item.put("qty", 1L + rnd(9));
                item.put("price", 5L + rnd(200));
                items.add(item);
            }
            Map<String, Object> order = new HashMap<>();
            order.put("id", 1000L + i);
            order.put("customer", buyers[(int) rnd(buyers.length)]);
            order.put("items", items);
            orders.add(order);
        }

        List<Function<Map<String, Object>, String>> tpl_invoice =
                compile("Invoice #{{id}} -- {{customer}}\n{{#items}}  {{name}} x{{qty}} @ {{price}}\n{{/items}}");
        List<Function<Map<String, Object>, String>> tpl_receipt =
                compile("Receipt {{id}} ({{customer}}): {{#items}}{{qty}}x{{name}};{{/items}}");
        List<Function<Map<String, Object>, String>> tpl_summary =
                compile("Summary {{customer}} {{missing_key}} {{#items}}{{name}},{{/items}}");

        long total = 0;
        for (int r = 0; r < rounds; r++) {
            for (Map<String, Object> o : orders) {
                String a = render(tpl_invoice, o);
                String b = render(tpl_receipt, o);
                String c = render(tpl_summary, o);
                total = (total + a.length() + b.length() + c.length()) % 1000003L;
            }
        }

        long sample = render(tpl_invoice, orders.get(0)).length();
        return new long[] {total, sample};
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
        check("warmup sample stable", warm[1], v[1]);
        check("checksum", v[0], 552394L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value sample: " + v[1]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
