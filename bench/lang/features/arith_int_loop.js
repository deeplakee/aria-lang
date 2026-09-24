// arith_int_loop 参照端口(与 arith_int_loop.aria 同算法同规模)。
const iterations = 5000000;

const t0 = performance.now();
let acc = 1;
let i = 0;
while (i < iterations) {
    acc = (acc + i) % 1000003;
    acc = acc * 3 - 1;
    acc += i % 7;
    i += 1;
}
const dt = performance.now() - t0;

check("arith_int_loop.checksum", acc, 109608);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
