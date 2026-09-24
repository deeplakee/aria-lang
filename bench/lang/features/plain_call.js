// plain_call 参照端口(与 plain_call.aria 同算法同规模)。
function step(v) {
    return v + 1;
}

const iterations = 6000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = acc + step(i);
    i += 1;
}
const dt = performance.now() - t0;

check("plain_call.checksum", acc, 18000003000000);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
