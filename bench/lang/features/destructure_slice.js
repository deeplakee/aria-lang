// destructure_slice 参照端口(与 destructure_slice.aria 同算法同规模)。
const iterations = 2000000;
const pair = [1, 2];
const xs = [10, 20, 30, 40, 50];

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    const [a, b] = pair;
    const head = xs.slice(1, 4);
    const [x, y] = head;
    acc = (acc + a + b + x + y) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("destructure_slice.checksum", acc, 999685);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
