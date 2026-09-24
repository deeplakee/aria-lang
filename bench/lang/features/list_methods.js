// list_methods 参照端口(与 list_methods.aria 同算法同规模)。
const size = 2000;
const rounds = 800000;

const xs = [];
for (let i = 0; i < size; i++) {
    xs.push(i);
}

const t0 = performance.now();
let acc = 0;
let r = 0;
while (r < rounds) {
    xs.push(r);
    xs.splice(0, 0, r);
    if (xs.includes(r)) {
        acc = (acc + xs.indexOf(r)) % 1000003;
    }
    acc = (acc + xs.splice(2, 1)[0]) % 1000003;
    acc = (acc + xs.pop()) % 1000003;
    acc = (acc + xs.length) % 1000003;
    r += 1;
}
const joined = xs.join(",");
acc = (acc + joined.length) % 1000003;
const dt = performance.now() - t0;

check("list_methods.checksum", acc, 684118);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
