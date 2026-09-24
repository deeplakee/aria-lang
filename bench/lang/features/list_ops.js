// list_ops 参照端口(与 list_ops.aria 同算法同规模)。
const size = 200000;
const rounds = 20;

const xs = [];
let i = 0;
while (i < size) {
    xs.push(i);
    i += 1;
}

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    let j = 0;
    while (j < size) {
        acc = (acc + xs[j] + xs[size - 1 - j]) % 1000003;
        xs[j] = (xs[j] + 1) % 1000003;
        j += 1;
    }
    r += 1;
}
acc = (acc + xs.length) % 1000003;
const dt = performance.now() - t0;

check("list_ops.checksum", acc, 799787);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
