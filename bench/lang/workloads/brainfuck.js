// brainfuck 参照端口(与 brainfuck.aria 同算法同规模):预处理 + 逐程序自校验在计时段外,
// t0 之后只有 rounds 轮 × 24 个已编译程序的重复执行段。
const suiteN = 24;   // 规模:生成程序数
const rounds = 70;   // 规模:执行轮数

let state = 91;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

function preprocess(src) {
    const code = [];
    const jumps = new Map();
    const stack = [];
    for (const c of src) {
        if ("+-<>.,[]".includes(c)) {
            if (c === "[") {
                stack.push(code.length);
            } else if (c === "]") {
                if (stack.length === 0) {
                    throw new Error("bf: unmatched ]");
                }
                const open = stack.pop();
                jumps.set(open, code.length);
                jumps.set(code.length, open);
            }
            code.push(c);
        }
    }
    if (stack.length !== 0) {
        throw new Error("bf: unmatched [");
    }
    return [code, jumps];
}

function runBf(code, jumps) {
    const tape = new Array(64).fill(0);
    let out = 0;
    let dp = 0;
    let pc = 0;
    const n = code.length;
    while (pc < n) {
        const c = code[pc];
        if (c === "-") {
            tape[dp] -= 1;
        } else if (c === "+") {
            tape[dp] += 1;
        } else if (c === "<") {
            dp -= 1;
        } else if (c === ">") {
            dp += 1;
        } else if (c === "[") {
            if (tape[dp] === 0) {
                pc = jumps.get(pc);
            }
        } else if (c === "]") {
            // 回跳后让公共的 pc += 1 恰好落在配对 [ 上重新判零
            pc = jumps.get(pc) - 1;
        } else if (c === ".") {
            out += tape[dp];
        }
        pc += 1;
    }
    return out;
}

// 乘法打印程序:cell0 = a,cell1 累加出 a*b 后逐次递减打印([.-] 输出 a*b..1 的码点)。
function makeMulProg(a, b) {
    let p = "+".repeat(a);
    p += "[>";
    p += "+".repeat(b);
    p += "<-]>";
    p += "[.-]";
    return p;
}

// 期望码点和 = a*b*(a*b+1)/2,封闭式自校验(计时段外)。
const sources = [];
const expected = [];
for (let i = 0; i < suiteN; i++) {
    const a = 4 + rnd(16);
    const b = 4 + rnd(16);
    sources.push(makeMulProg(a, b));
    expected.push(a * b * (a * b + 1) / 2);
}

const compiled = [];
for (let i = 0; i < sources.length; i++) {
    const cm = preprocess(sources[i]);
    if (runBf(cm[0], cm[1]) !== expected[i]) {
        console.error("bf: suite self-check failed");
        process.exit(1);
    }
    compiled.push(cm);
}

const t0 = performance.now();
let total = 0;
for (let r = 0; r < rounds; r++) {
    for (const cm of compiled) {
        total = (total + runBf(cm[0], cm[1])) % 1000003;
    }
}
check("brainfuck.checksum", total, 632355);
const dt = performance.now() - t0;

console.log("value checksum: " + total);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
