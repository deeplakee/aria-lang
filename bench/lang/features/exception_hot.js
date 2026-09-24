// exception_hot 参照端口(与 exception_hot.aria 同算法同规模):JS 可 throw 任意值,故载荷同 aria。
const iterations = 8000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    try {
        throw i;
    } catch (e) {
        acc = (acc + e) % 1000003;
    }
    i += 1;
}
const dt = performance.now() - t0;

check("exception_hot.checksum", acc, 300);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
