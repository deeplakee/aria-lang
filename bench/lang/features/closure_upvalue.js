// closure_upvalue 参照端口(与 closure_upvalue.aria 同算法同规模)。
function makeCounter() {
    let n = 0;
    return function () {
        n = n + 1;
        return n;
    };
}

const next = makeCounter();
const iterations = 6000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = acc + next();
    i += 1;
}
const dt = performance.now() - t0;

check("closure_upvalue.checksum", acc, 18000003000000);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
