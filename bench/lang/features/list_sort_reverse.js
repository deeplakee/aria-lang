// list_sort_reverse 参照端口(与 list_sort_reverse.aria 同算法同规模)。
const size = 200000;
const rounds = 20;

const xs = [];
let state = 1;
for (let i = 0; i < size; i++) {
    state = (state * 48271) % 2147483647;
    xs.push(state);
}

const t0 = performance.now();
let acc = 0;
for (let r = 0; r < rounds; r++) {
    xs.sort((left, right) => left - right);
    xs.reverse();
    for (let j = 0; j < size; j++) {
        acc = (acc * 31 + xs[j] % 1009) % 1000000007;
    }
}
const dt = performance.now() - t0;

check("list_sort_reverse.checksum", acc, 745687805);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
