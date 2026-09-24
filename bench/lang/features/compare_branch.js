// compare_branch 参照端口(与 compare_branch.aria 同算法同规模)。
const iterations = 4000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    if (i % 15 === 0) {
        acc += 3;
    } else if (i % 5 === 0) {
        acc += 2;
    } else if (i % 3 === 0) {
        acc += 1;
    } else {
        acc += 0;
    }
    i += 1;
}
const dt = performance.now() - t0;

check("compare_branch.checksum", acc, 2933334);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
