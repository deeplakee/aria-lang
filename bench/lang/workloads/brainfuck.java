// brainfuck 参照端口(与 brainfuck.aria 同算法同规模):suite 生成、预处理与逐程序自校验都在
// 计时段外,每遍 pass 的计时段只含 rounds 轮 x 24 个已编译程序的重复执行段;main 先整遍热身丢弃,
// 再计时跑第二遍报数。
class Bench_brainfuck {
    // 一次预处理的产物:过滤后的程序与括号配对表。
    static final class BfProgram {
        final char[] code;
        final java.util.HashMap<Integer, Integer> jumps;

        BfProgram(char[] code, java.util.HashMap<Integer, Integer> jumps) {
            this.code = code;
            this.jumps = jumps;
        }
    }

    static long state = 91;  // LCG 状态:pass() 每遍重置,两遍序列一致

    static int rnd(int n) {
        state = (state * 48271) % 2147483647L;
        return (int) (state % n);
    }

    static BfProgram preprocess(String src) {
        StringBuilder code = new StringBuilder();
        java.util.HashMap<Integer, Integer> jumps = new java.util.HashMap<>();
        java.util.ArrayDeque<Integer> stack = new java.util.ArrayDeque<>();
        for (int index = 0; index < src.length(); index++) {
            char c = src.charAt(index);
            if ("+-<>.,[]".indexOf(c) < 0) {
                continue;
            }
            if (c == '[') {
                stack.push(code.length());
            } else if (c == ']') {
                if (stack.isEmpty()) {
                    System.err.println("bf: unmatched ]");
                    System.exit(1);
                }
                int open = stack.pop();
                jumps.put(open, code.length());
                jumps.put(code.length(), open);
            }
            code.append(c);
        }
        if (!stack.isEmpty()) {
            System.err.println("bf: unmatched [");
            System.exit(1);
        }
        return new BfProgram(code.toString().toCharArray(), jumps);
    }

    static long run_bf(char[] code, java.util.HashMap<Integer, Integer> jumps) {
        int[] tape = new int[64];
        long out = 0;
        int dp = 0;
        int pc = 0;
        int n = code.length;
        while (pc < n) {
            char c = code[pc];
            if (c == '-') {
                tape[dp] -= 1;
            } else if (c == '+') {
                tape[dp] += 1;
            } else if (c == '<') {
                dp -= 1;
            } else if (c == '>') {
                dp += 1;
            } else if (c == '[') {
                if (tape[dp] == 0) {
                    pc = jumps.get(pc);
                }
            } else if (c == ']') {
                // 回跳后让公共的 pc += 1 恰好落在配对 [ 上重新判零
                pc = jumps.get(pc) - 1;
            } else if (c == '.') {
                out += tape[dp];
            }
            pc += 1;
        }
        return out;
    }

    // 乘法打印程序:cell0 = a,cell1 累加出 a*b 后逐次递减打印([.-] 输出 a*b..1 的码点)。
    static String make_mul_prog(int a, int b) {
        return "+".repeat(a) + "[>" + "+".repeat(b) + "<-]>" + "[.-]";
    }

    static double innerMs;  // 最近一遍 pass 的重复执行段耗时

    static long pass() {
        int suiteN = 24;   // 规模:生成程序数
        int rounds = 70;   // 规模:执行轮数
        String[] sources = new String[suiteN];
        long[] expected = new long[suiteN];
        state = 91;
        for (int i = 0; i < suiteN; i++) {
            int a = 4 + rnd(16);
            int b = 4 + rnd(16);
            sources[i] = make_mul_prog(a, b);
            expected[i] = a * b * (a * b + 1) / 2;  // 封闭式期望:码点和
        }
        java.util.ArrayList<BfProgram> compiled = new java.util.ArrayList<>();
        for (int i = 0; i < suiteN; i++) {
            BfProgram prog = preprocess(sources[i]);
            if (run_bf(prog.code, prog.jumps) != expected[i]) {
                System.err.println("bf: suite self-check failed");
                System.exit(1);
            }
            compiled.add(prog);
        }
        long t0 = System.nanoTime();
        long total = 0;
        for (int r = 0; r < rounds; r++) {
            for (BfProgram prog : compiled) {
                total = (total + run_bf(prog.code, prog.jumps)) % 1000003L;
            }
        }
        innerMs = (System.nanoTime() - t0) / 1e6;
        return total;
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long warm = pass();
        long total = pass();
        check("warmup stable", warm, total);
        check("checksum", total, 632355L);
        System.out.println("value checksum: " + total);
        System.out.println("bench-time: " + innerMs + " ms");
    }
}
