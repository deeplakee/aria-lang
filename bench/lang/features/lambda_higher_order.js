// lambda_higher_order 参照端口(与 lambda_higher_order.aria 同算法同规模)。
function compose(f, g) {
    return function (x) {
        return f(g(x));
    };
}

const inc = (x) => x + 1;
const dbl = (x) => x * 2;
const chain = compose(inc, dbl);
const iterations = 5000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = chain(acc) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("lambda_higher_order.checksum", acc, 395508);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
