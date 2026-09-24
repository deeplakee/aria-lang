// exception_deep_unwind 参照端口(与 exception_deep_unwind.aria 同算法同规模)。
function level3(v) {
    throw v;
}

function level2(v) {
    level3(v);
    return -1;
}

function level1(v) {
    level2(v);
    return -1;
}

const iterations = 2600000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    try {
        level1(i);
    } catch (e) {
        acc = (acc + e) % 1000003;
    }
    i += 1;
}
const dt = performance.now() - t0;

check("exception_deep_unwind.checksum", acc, 560036);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
