// list_slice 参照端口(与 list_slice.aria 同算法同规模):aria 的区间切片两端皆含,故 JS 侧上界 +1。
const size = 512;
const rounds = 900000;

const xs = [];
for (let i = 0; i < size; i++) {
    xs.push(i);
}

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    const a = xs.slice(10, 21);
    const b = xs.slice(-20);
    const c = xs.slice(10, 21).reverse();
    acc = (acc + a.length + b.length + c.length + a[0] + b[0] + c[0]) % 1000003;
    r += 1;
}
const dt = performance.now() - t0;

check("list_slice.checksum", acc, 598479);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
