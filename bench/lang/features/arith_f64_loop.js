// arith_f64_loop 参照端口(与 arith_f64_loop.aria 同算法同规模)。
const iterations = 5000000;

const t0 = performance.now();
let x = 0.5;
let hits = 0;
let i = 0;
while (i < iterations) {
    x = x * 1.0000001 + 0.25;
    if (x > 100.0) {
        x = x / 2.0;
    }
    if (x > 50.0) {
        hits += 1;
    }
    i += 1;
}
const dt = performance.now() - t0;

check("arith_f64_loop.checksum", hits, 4999803);
console.log("value checksum: " + hits);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
