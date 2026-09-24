// call_varargs 参照端口(与 call_varargs.aria 同算法同规模)。
function total(...xs) {
    let s = 0;
    for (const v of xs) {
        s = s + v;
    }
    return s;
}

const iterations = 1300000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = (acc + total(i, i + 1, i + 2)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("call_varargs.checksum", acc, 345018);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
