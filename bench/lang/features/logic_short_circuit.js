// logic_short_circuit 参照端口(与 logic_short_circuit.aria 同算法同规模)。
const iterations = 6000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    if ((i > 0 && i % 2 === 0) || i % 7 === 0) {
        acc = acc + 1;
    }
    i += 1;
}
const dt = performance.now() - t0;

check("logic_short_circuit.checksum", acc, 3428571);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
