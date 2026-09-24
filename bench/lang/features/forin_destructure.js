// forin_destructure 参照端口(与 forin_destructure.aria 同算法同规模)。
const size = 256;
const rounds = 13000;

const m = new Map();
for (let i = 0; i < size; i++) {
    m.set(i, i * 2);
}

const t0 = performance.now();
let acc = 0;
for (let r = 0; r < rounds; r++) {
    for (const [k, v] of m) {
        acc = (acc + k + v) % 1000003;
    }
}
const dt = performance.now() - t0;

check("forin_destructure.checksum", acc, 956184);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
