// class_inheritance_super 参照端口(与 class_inheritance_super.aria 同算法同规模)。
class Base {
    constructor(v) {
        this.v = v;
    }

    describe() {
        return 1;
    }
}

class Derived extends Base {
    constructor(v) {
        super(v);
    }

    describe() {
        return 10 + super.describe();
    }
}

const obj = new Derived(7);
const iterations = 4000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    acc = (acc + obj.describe()) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("class_inheritance_super.checksum", acc, 999871);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
