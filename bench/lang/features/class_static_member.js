// class_static_member 参照端口(与 class_static_member.aria 同算法同规模)。
class Counter {
    static n = 0;

    static add(d) {
        Counter.n = Counter.n + d;
        return Counter.n;
    }
}

const iterations = 4200000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = (acc + Counter.add(1)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("class_static_member.checksum", acc, 640075);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
