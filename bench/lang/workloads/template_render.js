// template_render 参照端口(与 template_render.aria 同算法同规模):mustache 式模板编译成捕获
// 片段的闭包列表(文本/变量/section 三种段,section 递归编译),对 LCG 造的订单数据反复渲染;
// 变量段做 HTML 转义与缺失键兜底。
const orders_n = 900;  // 规模:订单数
const rounds = 20;     // 规模:渲染轮数

let state = 2024;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

function esc_html(s) {
    return s.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

function lookup(d, name) {
    if (d.has(name)) {
        return d.get(name);
    }
    return null;
}

function find_from(src, needle, from) {
    // indexOf 带 fromIndex 即「在 src.substring(from) 里找」,命中返回 from + 局部偏移,未命中 -1。
    return src.indexOf(needle, from);
}

function render(segs, data) {
    let out = "";
    for (const seg of segs) {
        out += seg(data);
    }
    return out;
}

function compile(src) {
    const segs = [];
    let pos = 0;
    while (true) {
        const open = find_from(src, "{{", pos);
        if (open < 0) {
            const rest = src.substring(pos);
            if (rest.length > 0) {
                segs.push((d) => rest);
            }
            break;
        }
        if (open > pos) {
            const lit = src.substring(pos, open);
            segs.push((d) => lit);
        }
        const close = find_from(src, "}}", open);
        const tag = src.substring(open + 2, close);
        if (tag[0] === "#") {
            const name = tag.substring(1);
            const endtag = "{{/" + name + "}}";
            const end = find_from(src, endtag, close);
            const body = compile(src.substring(close + 2, end));
            segs.push((d) => {
                const items = lookup(d, name);
                if (items === null) {
                    return "";
                }
                let out = "";
                for (const item of items) {
                    out += render(body, item);
                }
                return out;
            });
            pos = end + endtag.length;
        } else {
            const name2 = tag;
            segs.push((d) => {
                const v = lookup(d, name2);
                if (v === null) {
                    return "";
                }
                return esc_html(String(v));
            });
            pos = close + 2;
        }
    }
    return segs;
}

const t0 = performance.now();

const item_names = ["widget", "gadget", "cable", "mount", "panel", "sensor"];
const buyers = ["acme", "globex", "initech", "umbrella", "stark", "wayne"];
const orders = [];
for (let i = 0; i < orders_n; i++) {
    const items = [];
    const n = 1 + rnd(4);
    for (let j = 0; j < n; j++) {
        const item = new Map();
        item.set("name", item_names[rnd(item_names.length)]);
        item.set("qty", 1 + rnd(9));
        item.set("price", 5 + rnd(200));
        items.push(item);
    }
    const order = new Map();
    order.set("id", 1000 + i);
    order.set("customer", buyers[rnd(buyers.length)]);
    order.set("items", items);
    orders.push(order);
}

const tpl_invoice = compile("Invoice #{{id}} -- {{customer}}\n{{#items}}  {{name}} x{{qty}} @ {{price}}\n{{/items}}");
const tpl_receipt = compile("Receipt {{id}} ({{customer}}): {{#items}}{{qty}}x{{name}};{{/items}}");
const tpl_summary = compile("Summary {{customer}} {{missing_key}} {{#items}}{{name}},{{/items}}");

let total = 0;
for (let r = 0; r < rounds; r++) {
    for (const o of orders) {
        const a = render(tpl_invoice, o);
        const b = render(tpl_receipt, o);
        const c = render(tpl_summary, o);
        total = (total + a.length + b.length + c.length) % 1000003;
    }
}

const sample = render(tpl_invoice, orders[0]).length;
const dt = performance.now() - t0;

check("template_render.checksum", total, 552394);

console.log("value checksum: " + total);
console.log("value sample: " + sample);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
