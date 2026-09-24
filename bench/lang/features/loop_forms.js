// loop_forms 参照端口(与 loop_forms.aria 同算法同规模)。
const rounds = 500000;

const t0 = performance.now();
let acc = 0;
for (let r = 0; r < rounds; r++) {
    for (let k = 0; k < 16; k += 1) {
        if (k % 3 === 0) {
            continue;
        }
        if (k > 12) {
            break;
        }
        acc = (acc + k) % 1000003;
    }
}
const dt = performance.now() - t0;

check("loop_forms.checksum", acc, 999931);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
