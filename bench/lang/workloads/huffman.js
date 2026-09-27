// huffman 参照端口(与 huffman.aria 同算法同规模)。建树队列按词首次出现序(不依赖 map 迭代序)。
const messages = 1150;
const msgLen = 80;

const letters = "etaoinshrdlucmfwypvbgkxjqz";
const weights = [113, 90, 80, 75, 70, 67, 63, 61, 60, 43, 40, 28, 24, 22, 21, 19, 19, 18, 17, 16, 15, 13, 10, 9, 5, 2];

let state = 31337;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

function genChar() {
    const r = rnd(1000);
    let acc = 0;
    for (let i = 0; i < weights.length; i++) {
        acc += weights[i];
        if (r < acc) {
            return letters[i];
        }
    }
    return letters[0];
}

const t0 = performance.now();

const texts = [];
for (let mi = 0; mi < messages; mi++) {
    const parts = [];
    for (let j = 0; j < msgLen; j++) {
        parts.push(genChar());
    }
    texts.push(parts.join(""));
}

const freq = new Map();
const order = [];
for (const msg of texts) {
    for (const ch of msg) {
        if (freq.has(ch)) {
            freq.set(ch, freq.get(ch) + 1);
        } else {
            freq.set(ch, 1);
            order.push(ch);
        }
    }
}

const pq = [];
for (const ch of order) {
    const n = freq.get(ch);
    let pos = pq.length;
    while (pos > 0 && pq[pos - 1].w > n) {
        pos -= 1;
    }
    pq.splice(pos, 0, {w: n, ch: ch, left: null, right: null});
}
while (pq.length > 1) {
    const a = pq.shift();
    const b = pq.shift();
    const merged = {w: a.w + b.w, ch: "", left: a, right: b};
    let pos = pq.length;
    while (pos > 0 && pq[pos - 1].w > merged.w) {
        pos -= 1;
    }
    pq.splice(pos, 0, merged);
}
const root = pq[0];

const codes = new Map();
function walk(node, prefix) {
    if (node.left === null) {
        codes.set(node.ch, prefix);
        return;
    }
    walk(node.left, prefix + "0");
    walk(node.right, prefix + "1");
}
walk(root, "");

let bitTotal = 0;
const encoded = [];
for (const msg of texts) {
    const bits = [];
    for (const ch of msg) {
        const code = codes.get(ch);
        bitTotal += code.length;
        for (let j = 0; j < code.length; j++) {
            bits.push(code[j]);
        }
    }
    encoded.push(bits.join(""));
}

let decodedOk = 0;
for (let di = 0; di < messages; di++) {
    const enc = encoded[di];
    let node = root;
    const out = [];
    for (let i = 0; i < enc.length; i++) {
        if (enc[i] === "0") {
            node = node.left;
        } else {
            node = node.right;
        }
        if (node.left === null) {
            out.push(node.ch);
            node = root;
        }
    }
    if (out.join("") === texts[di]) {
        decodedOk += 1;
    }
}

check("huffman.symbols", codes.size, 26);
check("huffman.bit_total", bitTotal, 395969);
check("huffman.decoded_ok", decodedOk, messages);

const dt = performance.now() - t0;

console.log("value symbols: " + codes.size);
console.log("value bit_total: " + bitTotal);
console.log("value decoded_ok: " + decodedOk);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
