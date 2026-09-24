// sort_int 参照端口(与 sort_int.aria 同算法同规模)。
const n = 2500000;

const t0 = performance.now();
const xs = [];
let state = 1;
for (let i = 0; i < n; i++) {
    state = (state * 48271) % 2147483647;
    xs.push(state);
}
xs.sort((left, right) => left - right);

let checksum = 0;
for (let i = 0; i < n; i++) {
    checksum = (checksum * 31 + xs[i] % 1009) % 1000000007;
}
const dt = performance.now() - t0;

check("sort_int.checksum", checksum, 947379667);
check("sort_int.min", xs[0], 145);
check("sort_int.max", xs[n - 1], 2147483426);

console.log("value checksum: " + checksum);
console.log("value min: " + xs[0]);
console.log("value max: " + xs[n - 1]);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
