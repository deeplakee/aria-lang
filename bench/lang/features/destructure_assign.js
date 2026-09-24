// destructure_assign 参照端口(与 destructure_assign.aria 同算法同规模)。
const rounds = 1200000;

let p = 1;
let q = 2;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < rounds) {
    [p, q] = [q, p + q];
    p = p % 1000003;
    q = q % 1000003;
    const [x, ...rest] = [p, q, p + q];
    acc = (acc + x + rest.length) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("destructure_assign.checksum", acc, 180840);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
