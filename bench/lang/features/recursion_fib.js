// recursion_fib 参照端口(与 recursion_fib.aria 同算法同规模)。
function fib(n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

const rounds = 20;
const fibInput = 27;

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    acc += fib(fibInput);
    r += 1;
}
const dt = performance.now() - t0;

check("recursion_fib.checksum", acc, 3928360);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
