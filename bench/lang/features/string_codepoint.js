// string_codepoint 参照端口(与 string_codepoint.aria 同算法同规模):码点域;字节域口径各语言
// 不同,不进校验和。
const rounds = 1100000;
const base = "héllo wörld";

const t0 = performance.now();
let acc = 0;
let i = 0;
while (i < rounds) {
    const cs = [...base];
    acc = (acc + cs.length + base.codePointAt(1) + base.codePointAt(4) + cs[0].codePointAt(0)) % 1000003;
    i += 1;
}
const dt = performance.now() - t0;

check("string_codepoint.checksum", acc, 898488);
check("string_codepoint.codepoints", [...base].length, 11);
console.log("value checksum: " + acc);
console.log("value codepoints: " + [...base].length);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
