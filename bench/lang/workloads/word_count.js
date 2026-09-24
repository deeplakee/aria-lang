// word_count 参照端口(与 word_count.aria 同算法同规模)。
const vocab = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta"];
const words = 3000000;

const t0 = performance.now();
let state = 1;
const parts = [];
for (let i = 0; i < words; i++) {
    state = (state * 48271) % 2147483647;
    parts.push(vocab[state % 8]);
}
const text = parts.join(" ");

const counts = new Map();
for (const w of text.split(" ")) {
    if (counts.has(w)) {
        counts.set(w, counts.get(w) + 1);
    } else {
        counts.set(w, 1);
    }
}

let total = 0;
for (const w of vocab) {
    total += counts.get(w);
}
const dt = performance.now() - t0;

check("word_count.total", total, 3000000);
check("word_count.alpha", counts.get("alpha"), 374756);
check("word_count.theta", counts.get("theta"), 374622);

console.log("value checksum: " + total);
console.log("value alpha: " + counts.get("alpha"));
console.log("value theta: " + counts.get("theta"));
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
