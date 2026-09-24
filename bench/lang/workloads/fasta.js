// fasta 参照端口(与 fasta.aria 同算法同规模)。
const im = 139968;
const ia = 3877;
const ic = 29573;
let state = 42;

const chars = ["a", "c", "g", "t", "B", "D", "H", "K", "M", "N", "R", "S", "V", "W", "Y"];
const probs = [0.27, 0.12, 0.12, 0.27, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02, 0.02];
const cum = [];
let running = 0.0;
for (const p of probs) {
    running = running + p;
    cum.push(running);
}

const bases = 2000000;

const t0 = performance.now();
const parts = [];
let checksum = 0;
for (let i = 0; i < bases; i++) {
    state = (state * ia + ic) % im;
    const pick = state * 1.0 / im;
    let idx = 0;
    while (idx < 15 && cum[idx] <= pick) {
        idx += 1;
    }
    if (idx === 15) {
        idx = 14;
    }
    parts.push(chars[idx]);
    checksum = (checksum * 131 + chars[idx].charCodeAt(0)) % 1000000007;
}
const seq = parts.join("");
const dt = performance.now() - t0;

check("fasta.checksum", checksum, 575313243);
check("fasta.length", seq.length, 2000000);

console.log("value checksum: " + checksum);
console.log("value length: " + seq.length);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
