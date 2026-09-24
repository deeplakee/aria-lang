// string_methods 参照端口(与 string_methods.aria 同算法同规模):aria 的 replace 换掉全部命中,
// 故 JS 用 replaceAll(单数的 replace 只换第一处)。
const rounds = 450000;
const base = "  the quick brown fox jumps over the lazy dog  ";

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < rounds) {
    const t = base.trim();
    const parts = t.split(" ");
    const joined = parts.join("-");
    const up = t.toUpperCase();
    if (t.startsWith("the") && t.endsWith("dog")) {
        acc = acc + 1;
    }
    acc = (acc + t.indexOf("fox") + t.replaceAll("o", "0").length + joined.length + up.length +
           t.substring(4, 9).length) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("string_methods.checksum", acc, 949799);
console.log("value checksum: " + acc);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
