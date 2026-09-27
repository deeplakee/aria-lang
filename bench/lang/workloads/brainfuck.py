"""brainfuck 参照端口(与 brainfuck.aria 同算法同规模):Brainfuck 解释器 -- 预处理(剔注释字符 +
括号配对表)后,字符级取指、纸带读写解释执行一组 LCG 造的乘法打印程序;套件先逐程序按封闭式期望值
自校验,再进重复执行段。规模见 suite_n/rounds。"""
import time

suite_n = 24   # 规模:生成程序数
rounds = 70    # 规模:执行轮数

state = 91


def rnd(n):
    global state
    state = (state * 48271) % 2147483647
    return state % n


def preprocess(src):
    code = []
    jumps = {}
    stack = []
    for c in src:
        if c in "+-<>.,[]":
            if c == "[":
                stack.append(len(code))
            elif c == "]":
                if len(stack) == 0:
                    raise RuntimeError("bf: unmatched ]")
                open_index = stack.pop()
                jumps[open_index] = len(code)
                jumps[len(code)] = open_index
            code.append(c)
    if len(stack) != 0:
        raise RuntimeError("bf: unmatched [")
    return code, jumps


def run_bf(code, jumps):
    tape = [0] * 64
    out = 0
    dp = 0
    pc = 0
    n = len(code)
    while pc < n:
        c = code[pc]
        if c == "-":
            tape[dp] -= 1
        elif c == "+":
            tape[dp] += 1
        elif c == "<":
            dp -= 1
        elif c == ">":
            dp += 1
        elif c == "[":
            if tape[dp] == 0:
                pc = jumps[pc]
        elif c == "]":
            # 回跳后让公共的 pc += 1 恰好落在配对 [ 上重新判零
            pc = jumps[pc] - 1
        elif c == ".":
            out += tape[dp]
        pc += 1
    return out


def make_mul_prog(a, b):
    # 乘法打印程序:cell0 = a,cell1 累加出 a*b 后逐次递减打印([.-] 输出 a*b..1 的码点)。
    p = "+" * a
    p += "[>"
    p += "+" * b
    p += "<-]>"
    p += "[.-]"
    return p


# 期望码点和 = a*b*(a*b+1)/2,封闭式自校验(计时段外)。
sources = []
expected = []
for _ in range(suite_n):
    a = 4 + rnd(16)
    b = 4 + rnd(16)
    sources.append(make_mul_prog(a, b))
    expected.append(a * b * (a * b + 1) // 2)

compiled = []
for i in range(len(sources)):
    code, jumps = preprocess(sources[i])
    if run_bf(code, jumps) != expected[i]:
        raise RuntimeError("bf: suite self-check failed")
    compiled.append((code, jumps))

t0 = time.perf_counter()
total = 0
for _ in range(rounds):
    for code, jumps in compiled:
        total = (total + run_bf(code, jumps)) % 1000003
assert total == 632355, "brainfuck checksum drift"
dt = (time.perf_counter() - t0) * 1000.0

print("value checksum:", total)
print("bench-time:", dt, "ms")
