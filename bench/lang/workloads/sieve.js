// sieve 参照端口(与 sieve.aria 同算法同规模);字节容器用本语言自然的 Uint8Array。
const t0 = performance.now();
let total = 0;
const counts = [];
for (const n of [100000, 1000000, 4000000]) {
    const composite = new Uint8Array(n);
    let count = 0;
    for (let i = 2; i < n; i++) {
        if (composite[i] === 0) {
            count += 1;
            for (let j = i * i; j < n; j += i) {
                composite[j] = 1;
            }
        }
    }
    total += count;
    counts.push(count);
    console.log("value count_" + n + ": " + count);
}
const dt = performance.now() - t0;

check("sieve.count_100000", counts[0], 9592);
check("sieve.count_1000000", counts[1], 78498);
check("sieve.count_4000000", counts[2], 283146);
check("sieve.total", total, 371236);

console.log("value checksum: " + total);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
