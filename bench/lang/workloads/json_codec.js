// json_codec 参照端口(与 json_codec.aria 同算法同规模):生成嵌套文档并序列化,再走
// 「解析 -> 再序列化 -> 逐位比对」往返,外加剧段截断片段的合法性校验。解析器手写递归下降,
// 禁用内建 JSON;序列化按 canonical 约定对对象键排序;JS 对 obj[k] = v 先求左侧引用,
// 与 aria 一致,键的 rnd 先于值的求值。
const doc_count = 120;  // 规模:文档数
const passes = 20;      // 规模:往返遍数
const snippets = 2000;  // 规模:校验片段数

let state = 7;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

const names = ["id", "name", "count", "active", "tags", "meta", "score", "note", "kind", "level"];
const words = ["core", "edge", "node", "wire", "mesh", "flux", "grid", "pulse"];

function gen_string() {
    const n = 2 + rnd(4);
    let out = "";
    for (let i = 0; i < n; i++) {
        out += words[rnd(words.length)];
        const k = rnd(9);
        if (k === 0) {
            out += "\\";
        } else if (k === 1) {
            out += "\"";
        } else if (k === 2) {
            out += "\n";
        }
    }
    return out;
}

function gen_value(depth) {
    const t = rnd(10);
    if (depth >= 3 || t < 3) {
        return rnd(100000);
    }
    if (t === 3) {
        return gen_string();
    }
    if (t === 4) {
        return null;
    }
    if (t <= 6) {
        const arr = [];
        const n = 1 + rnd(4);
        for (let i = 0; i < n; i++) {
            arr.push(gen_value(depth + 1));
        }
        return arr;
    }
    const obj = {};
    const n = 1 + rnd(3);
    for (let i = 0; i < n; i++) {
        obj[names[rnd(names.length)]] = gen_value(depth + 1);
    }
    return obj;
}

function gen_doc() {
    const obj = {};
    const n = 3 + rnd(4);
    for (let i = 0; i < n; i++) {
        obj[names[rnd(names.length)]] = gen_value(1);
    }
    return obj;
}

function escape_json(s) {
    let out = "\"";
    for (let i = 0; i < s.length; i++) {
        const c = s[i];
        if (c === "\"" || c === "\\") {
            out += "\\" + c;
        } else if (c === "\n") {
            out += "\\n";
        } else if (c === "\t") {
            out += "\\t";
        } else {
            out += c;
        }
    }
    return out + "\"";
}

function json_stringify(v) {
    if (v === null) {
        return "null";
    }
    const t = typeof v;
    if (t === "number") {
        return String(v);
    }
    if (t === "string") {
        return escape_json(v);
    }
    if (Array.isArray(v)) {
        let out = "[";
        let first = true;
        for (const item of v) {
            if (!first) {
                out += ",";
            }
            first = false;
            out += json_stringify(item);
        }
        return out + "]";
    }
    let out = "{";
    const ks = Object.keys(v);
    ks.sort();
    let first = true;
    for (const k of ks) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += escape_json(k) + ":" + json_stringify(v[k]);
    }
    return out + "}";
}

class JsonParser {
    constructor(src) {
        this.src = src;
        this.pos = 0;
    }

    fail(msg) {
        throw new Error("json: " + msg);
    }

    skip_ws() {
        while (this.pos < this.src.length) {
            const c = this.src[this.pos];
            if (c === " " || c === "\n" || c === "\t" || c === "\r") {
                this.pos += 1;
            } else {
                break;
            }
        }
    }

    peek() {
        if (this.pos >= this.src.length) {
            this.fail("unexpected end");
        }
        return this.src[this.pos];
    }

    expect(ch) {
        if (this.peek() !== ch) {
            this.fail("expected '" + ch + "' at " + this.pos);
        }
        this.pos += 1;
    }

    parse_lit(word, value) {
        const end = this.pos + word.length;
        if (end > this.src.length || this.src.substring(this.pos, end) !== word) {
            this.fail("bad literal");
        }
        this.pos = end;
        return value;
    }

    parse_number() {
        const start = this.pos;
        while (this.pos < this.src.length) {
            const c = this.src[this.pos];
            if (c === "-" || (c >= "0" && c <= "9")) {
                this.pos += 1;
            } else {
                break;
            }
        }
        if (this.pos === start) {
            this.fail("bad number at " + start);
        }
        const s = this.src.substring(start, this.pos);
        if (!/^-?\d+$/.test(s) || Number.isNaN(parseInt(s, 10))) {
            this.fail("bad number at " + start);
        }
        return parseInt(s, 10);
    }

    parse_string() {
        this.expect("\"");
        let out = "";
        while (true) {
            if (this.pos >= this.src.length) {
                this.fail("unterminated string");
            }
            const c = this.src[this.pos];
            if (c === "\"") {
                this.pos += 1;
                return out;
            }
            if (c === "\\") {
                this.pos += 1;
                const e = this.peek();
                if (e === "n") {
                    out += "\n";
                } else if (e === "t") {
                    out += "\t";
                } else {
                    out += e;
                }
                this.pos += 1;
                continue;
            }
            out += c;
            this.pos += 1;
        }
    }

    parse_array() {
        this.expect("[");
        const arr = [];
        this.skip_ws();
        if (this.peek() === "]") {
            this.pos += 1;
            return arr;
        }
        while (true) {
            arr.push(this.parse_value());
            this.skip_ws();
            if (this.peek() === ",") {
                this.pos += 1;
                continue;
            }
            this.expect("]");
            return arr;
        }
    }

    parse_object() {
        this.expect("{");
        const obj = {};
        this.skip_ws();
        if (this.peek() === "}") {
            this.pos += 1;
            return obj;
        }
        while (true) {
            const key = this.parse_string();
            this.skip_ws();
            this.expect(":");
            obj[key] = this.parse_value();
            this.skip_ws();
            if (this.peek() === ",") {
                this.pos += 1;
                continue;
            }
            this.expect("}");
            return obj;
        }
    }

    parse_value() {
        this.skip_ws();
        const c = this.peek();
        if (c === "{") {
            return this.parse_object();
        }
        if (c === "[") {
            return this.parse_array();
        }
        if (c === "\"") {
            return this.parse_string();
        }
        if (c === "n") {
            return this.parse_lit("null", null);
        }
        return this.parse_number();
    }
}

function parse_json(src) {
    const p = new JsonParser(src);
    const v = p.parse_value();
    p.skip_ws();
    if (p.pos !== src.length) {
        p.fail("trailing content");
    }
    return v;
}

function count_nodes(v) {
    if (Array.isArray(v)) {
        let n = 1;
        for (const item of v) {
            n += count_nodes(item);
        }
        return n;
    }
    if (v !== null && typeof v === "object") {
        let n = 1;
        for (const k of Object.keys(v)) {
            n += count_nodes(v[k]);
        }
        return n;
    }
    return 1;
}

const t0 = performance.now();

const docs = [];
for (let i = 0; i < doc_count; i++) {
    docs.push(json_stringify(gen_doc()));
}

let node_acc = 0;
let roundtrip_ok = 0;
for (let p = 0; p < passes; p++) {
    for (const text of docs) {
        const v = parse_json(text);
        if (json_stringify(v) === text) {
            roundtrip_ok += 1;
        }
        node_acc = (node_acc + count_nodes(v)) % 1000003;
    }
}

let valid = 0;
let invalid = 0;
for (let i = 0; i < snippets; i++) {
    const text = docs[i % doc_count];
    let probe = text.substring(0, 1 + rnd(Math.floor(text.length / 2)));
    if (rnd(3) === 0) {
        probe = probe + "}";
    } else if (rnd(3) === 0) {
        probe = probe + text[0];
    }
    try {
        parse_json(probe);
        valid += 1;
    } catch (e) {
        invalid += 1;
    }
}

const dt = performance.now() - t0;

check("json_codec.roundtrip_total", roundtrip_ok, doc_count * passes);
check("json_codec.roundtrip_ok", roundtrip_ok, 2400);
check("json_codec.node_acc", node_acc, 31480);
check("json_codec.valid", valid, 58);
check("json_codec.invalid", invalid, 1942);

console.log("value roundtrip_ok: " + roundtrip_ok);
console.log("value node_acc: " + node_acc);
console.log("value valid: " + valid);
console.log("value invalid: " + invalid);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("value drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
