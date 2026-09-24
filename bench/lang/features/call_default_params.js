// call_default_params 参照端口(与 call_default_params.aria 同算法同规模)。
function step(v, k = 3) {
    return v + k;
}

const iterations = 5500000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = (acc + step(i)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("call_default_params.checksum", acc, 375096);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
