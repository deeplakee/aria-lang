// string_ops 参照端口(与 string_ops.aria 同算法同规模)。
const rounds = 1000000;
const base = "the quick brown fox jumps over the lazy dog";

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < rounds) {
    if (base.includes("brown")) {
        acc += base.indexOf("fox");
    }
    const parts = base.split(" ");
    acc = (acc + parts.length + base.substring(4, 9).length) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("string_ops.checksum", acc, 999913);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
