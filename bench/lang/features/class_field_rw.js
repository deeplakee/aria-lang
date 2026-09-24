// class_field_rw 参照端口(与 class_field_rw.aria 同算法同规模)。
class Cell {
    constructor() {
        this.v = 0;
    }
}

const cell = new Cell();
const iterations = 7000000;

const t0 = performance.now();
let i = 0;
while (i < iterations) {
    cell.v = cell.v + 1;
    i += 1;
}
const acc = cell.v;
const dt = performance.now() - t0;

check("class_field_rw.checksum", acc, 7000000);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
