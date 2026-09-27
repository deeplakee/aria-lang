// word_diff 参照端口(与 word_diff.aria 同算法同规模):LCG 造基准文档,按比例做替换/插词/删词得
// 修订版,再对每对版本跑 LCS 全表动态规划 + 回溯,统计保留/删除/插入词数(评审工具的词级 diff)。
const docSize = 130; // 规模:文档词数
const pairs = 42;    // 规模:版本对数

const vocab = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta", "theta", "iota", "kappa",
               "lambda", "mu"];

let state = 555;

function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

function genDoc() {
    const d = [];
    for (let i = 0; i < docSize; i++) {
        d.push(vocab[rnd(vocab.length)]);
    }
    return d;
}

function mutate(src) {
    const out = [];
    for (let i = 0; i < src.length; i++) {
        const roll = rnd(20);
        if (roll === 0) {
            // 删词:跳过
        } else if (roll === 1) {
            out.push(vocab[rnd(vocab.length)]);
        } else if (roll === 2) {
            out.push(src[i]);
            out.push(vocab[rnd(vocab.length)]);
        } else {
            out.push(src[i]);
        }
    }
    return out;
}

function diffCounts(a, b) {
    const n1 = a.length;
    const n2 = b.length;
    const t = [];
    for (let i = 0; i <= n1; i++) {
        const row = [];
        for (let j = 0; j <= n2; j++) {
            row.push(0);
        }
        t.push(row);
    }
    for (let i = 1; i <= n1; i++) {
        for (let j = 1; j <= n2; j++) {
            if (a[i - 1] === b[j - 1]) {
                t[i][j] = t[i - 1][j - 1] + 1;
            } else {
                const up = t[i - 1][j];
                const left = t[i][j - 1];
                t[i][j] = up >= left ? up : left;
            }
        }
    }
    let eq = 0;
    let del = 0;
    let ins = 0;
    let i = n1;
    let j = n2;
    while (i > 0 && j > 0) {
        if (a[i - 1] === b[j - 1]) {
            eq += 1;
            i -= 1;
            j -= 1;
        } else if (t[i - 1][j] >= t[i][j - 1]) {
            del += 1;
            i -= 1;
        } else {
            ins += 1;
            j -= 1;
        }
    }
    del += i;
    ins += j;
    return [eq, del, ins];
}

const t0 = performance.now();
let eqSum = 0;
let delSum = 0;
let insSum = 0;
for (let p = 0; p < pairs; p++) {
    const a = genDoc();
    const b = mutate(a);
    const [eq, del, ins] = diffCounts(a, b);
    eqSum = (eqSum + eq) % 1000003;
    delSum = (delSum + del) % 1000003;
    insSum = (insSum + ins) % 1000003;
}
const dt = performance.now() - t0;

check("word_diff.eq_sum", eqSum, 4966);
check("word_diff.del_sum", delSum, 494);
check("word_diff.ins_sum", insSum, 503);

console.log("value eq_sum: " + eqSum);
console.log("value del_sum: " + delSum);
console.log("value ins_sum: " + insSum);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
