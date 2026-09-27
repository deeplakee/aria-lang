// markov 参照端口(与 markov.aria 同算法同规模)。
const sentences = 6500;
const genWords = 20000;

const vocab = ["the", "of", "and", "a", "to", "in", "is", "it", "was", "that", "for", "time",
               "world", "life", "mind", "light", "river", "stone", "forest", "winter", "summer",
               "harbor", "signal", "memory", "shadow", "window", "garden", "mirror", "voyage", "silence"];

let state = 12345;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

const t0 = performance.now();

const corpus = [];
for (let i = 0; i < sentences; i++) {
    const len = 6 + rnd(9);
    for (let j = 0; j < len; j++) {
        corpus.push(vocab[rnd(vocab.length)]);
    }
}

const states = new Map();
let w0 = corpus[0];
let w1 = corpus[1];
for (let i = 2; i < corpus.length; i++) {
    const key = w0 + " " + w1;
    let row = states.get(key);
    if (row === undefined) {
        row = [];
        states.set(key, row);
    }
    const w2 = corpus[i];
    let found = false;
    let p = 0;
    while (p < row.length) {
        if (row[p] === w2) {
            row[p + 1] += 1;
            found = true;
        }
        p += 2;
    }
    if (!found) {
        row.push(w2);
        row.push(1);
    }
    w0 = w1;
    w1 = w2;
}

const produced = [];
let checksum = 0;
let g0 = corpus[0];
let g1 = corpus[1];
let made = 0;
while (made < genWords) {
    const row = states.get(g0 + " " + g1);
    if (row === undefined) {
        g0 = corpus[0];
        g1 = corpus[1];
        continue;
    }
    let total = 0;
    let p = 1;
    while (p < row.length) {
        total += row[p];
        p += 2;
    }
    let pick = rnd(total);
    let w = row[0];
    p = 1;
    while (p < row.length) {
        if (pick < row[p]) {
            w = row[p - 1];
            break;
        }
        pick -= row[p];
        p += 2;
    }
    produced.push(w);
    checksum = (checksum * 31 + w.length) % 1000003;
    g0 = g1;
    g1 = w;
    made += 1;
}

const seen = new Set(produced);

check("markov.states", states.size, 900);
check("markov.checksum", checksum, 679528);
check("markov.distinct", seen.size, 30);

const dt = performance.now() - t0;

console.log("value states: " + states.size);
console.log("value checksum: " + checksum);
console.log("value distinct: " + seen.size);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
