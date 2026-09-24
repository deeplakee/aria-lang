// matmul_int 参照端口(与 matmul_int.aria 同算法同规模)。
const n = 250;

const t0 = performance.now();
const a = [];
for (let i = 0; i < n; i++) {
    const row = [];
    for (let j = 0; j < n; j++) {
        row.push((i * 7 + j * 13 + 1) % 1009);
    }
    a.push(row);
}
const b = [];
for (let i = 0; i < n; i++) {
    const row = [];
    for (let j = 0; j < n; j++) {
        row.push((i * 11 + j * 3 + 2) % 1009);
    }
    b.push(row);
}

let checksum = 0;
for (let i = 0; i < n; i++) {
    for (let j = 0; j < n; j++) {
        let acc = 0;
        for (let k = 0; k < n; k++) {
            acc += a[i][k] * b[k][j];
        }
        checksum = (checksum * 31 + acc) % 1000000007;
    }
}
const dt = performance.now() - t0;

check("matmul_int.checksum", checksum, 396310743);

console.log("value checksum: " + checksum);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
