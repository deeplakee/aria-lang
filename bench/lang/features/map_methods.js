// map_methods 参照端口(与 map_methods.aria 同算法同规模):快照序 unspecified,只做与序无关的
// 求和与计数(与 aria 端同一算法)。
const entries = 20000;
const rounds = 1600;

const m = new Map();
for (let i = 0; i < entries; i++) {
    m.set(i, i * 2);
}

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    const key = r % entries;
    m.delete(key);
    m.set(key, r);
    if (m.has(key)) {
        acc = (acc + m.get(key)) % 1000003;
    }
    acc = (acc + m.size) % 1000003;
    if (r % 10 === 0) {
        const ks = [...m.keys()];
        const vs = [...m.values()];
        const ps = [...m.entries()];
        acc = (acc + ks.length + vs.length + ps.length) % 1000003;
        for (let j = 0; j < ks.length; j++) {
            acc = (acc + ks[j]) % 1000003;
        }
    }
    r += 1;
}
m.clear();
acc = (acc + m.size) % 1000003;
const dt = performance.now() - t0;

check("map_methods.checksum", acc, 183077);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
