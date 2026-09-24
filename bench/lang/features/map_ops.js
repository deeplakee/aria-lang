// map_ops 参照端口(与 map_ops.aria 同算法同规模)。
const entries = 100000;
const rounds = 40;

const m = new Map();
let i = 0;
while (i < entries) {
    m.set(i, i * 2);
    i += 1;
}

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    let j = 0;
    while (j < entries) {
        if (m.has(j)) {
            acc = (acc + m.get(j)) % 1000003;
        }
        j += 1;
    }
    r += 1;
}
acc = (acc + m.size) % 1000003;
const dt = performance.now() - t0;

check("map_ops.checksum", acc, 900018);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
