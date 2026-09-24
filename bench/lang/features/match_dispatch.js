// match_dispatch 参照端口(与 match_dispatch.aria 同算法同规模):JS 无 match 表达式,用本语言的
// 多路派发写法 switch。
const iterations = 4000000;

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < iterations) {
    let bucket;
    switch (i % 5) {
        case 0: bucket = 3; break;
        case 1: bucket = 5; break;
        case 2: bucket = 7; break;
        case 3: bucket = 11; break;
        default: bucket = 13; break;
    }
    acc = (acc + bucket) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("match_dispatch.checksum", acc, 199907);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
