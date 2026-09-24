// method_dispatch 参照端口(与 method_dispatch.aria 同算法同规模)。
class Counter {
    constructor() {
        this.base = 1;
    }

    step(delta) {
        return this.base + delta;
    }
}

const counter = new Counter();
const iterations = 5000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = (acc + counter.step(i)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("method_dispatch.checksum", acc, 105);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
