// bound_method_read 参照端口(与 bound_method_read.aria 同算法同规模):JS 取出的方法值会丢 this,
// 故用本语言的绑定写法 bind(每轮一次绑定,与 aria 读路径现场绑定同性质)。
class Cell {
    constructor(v) {
        this.v = v;
    }

    step(d) {
        return this.v + d;
    }
}

const obj = new Cell(1);
const iterations = 3800000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    const f = obj.step.bind(obj);
    acc = (acc + f(i)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("bound_method_read.checksum", acc, 240060);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
