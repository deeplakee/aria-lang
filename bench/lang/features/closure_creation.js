// closure_creation 参照端口(与 closure_creation.aria 同算法同规模)。
function make(base) {
    return (d) => base + d;
}

const iterations = 2800000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    const f = make(i);
    acc = (acc + f(2)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("closure_creation.checksum", acc, 440024);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
