// scope_locals 参照端口(与 scope_locals.aria 同算法同规模)。
function work(a) {
    const b = a + 1;
    const c = b * 2;
    const d = c - 3;
    let out = d;
    {
        const e = d + 4;
        out = e % 1000003;
    }
    return out;
}

const rounds = 4500000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < rounds) {
    acc = (acc + work(i)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("scope_locals.checksum", acc, 250156);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
