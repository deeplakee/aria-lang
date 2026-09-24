// iter_protocol 参照端口(与 iter_protocol.aria 同算法同规模)。
const size = 512;
const rounds = 4000;

const xs = [];
for (let i = 0; i < size; i++) {
    xs.push(i);
}
const m = new Map();
for (let k = 0; k < size; k++) {
    m.set(k, k);
}

const t0 = performance.now();
let acc = 0;
for (let r = 0; r < rounds; r++) {
    for (const v of xs) {
        acc = (acc + v) % 1000003;
    }
    for (let v = 0; v < size; v++) {
        acc = (acc + v) % 1000003;
    }
    for (const _ch of "abc") {
        acc = acc + 1;
    }
    for (const k of m.keys()) {
        acc = (acc + k) % 1000003;
    }
}
const dt = performance.now() - t0;

check("iter_protocol.checksum", acc, 799293);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
