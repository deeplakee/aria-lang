// bench/vm_bench.cpp
//
// VM 层性能基准：方法派发 / 迭代协议 / 调用开销。这些数字是「不绑定派发」取舍的依据（解读见
// .claude/reference/bytecode/bytecode-instruction-set.md §6.2），
// 本程序是它们的可复现入口--改 CodeGen 发射或 VM 派发路径前后各跑一次对照，把新数字回写文档。
//
// 每行压一种派发形态，迭代次数统一 kIterations（循环体共 1,638,400 次），故 ns/次可跨行比较：
//   forin_list / forin_range  迭代协议：iter/has_next/next 各一次 `PREPARE_METHOD` + `CALL_METHOD`
//   starts_with               单方法调用：一次 `PREPARE_METHOD` + `CALL_METHOD`（不涉迭代协议）
//   instance_call/instance_read  用户类实例方法的调用与**读取**:调用走「类链查表 + 不绑定」,
//                              读取每次现场绑定(方法值是一等值,必须产出 bound 对象)
//   instance_operator         用户类算子重载:`a + b`(ADD -> 按名取实现 -> 调用) 对 `a.__add__(b)`
//                             (名字由常量池给出),两形态只差「名字从哪来」
//   plain_call                普通函数调用：无 `LOAD_FIELD` 的调用下界
//   forin_string / forin_map  string 逐码点、map 逐 pair 迭代（每次迭代另有对象分配，非纯派发成本）
//
// 前三行是「同一程序两形态」的**同二进制内对照**，本基准的核心手法：
//   - 协议形态：源码写 `recv.name(args)`（现编译为 `PREPARE_METHOD` + `CALL_METHOD` 两段，零 bound 物化）
//   - 预绑定形态：先把方法值读到局部（`var nx = it.next`）再调 `nx()`--循环体内零 `LOAD_FIELD`、
//     零 ObjBoundMethod，即「不绑定派发能达到的下界」。预绑定不是正常写法，只作对照。
//   两形态算同一个结果（BENCH_CHECK 断言相等），差值 = 每次调用花在 `LOAD_FIELD` + 绑定 + 多一次
//   dispatch 上的成本，也就是不绑定派发可回收的**上界**（两段式 PREPARE_METHOD + CALL_METHOD 相对两步
//   形态节省的量;两段式自身的开销见 bytecode-instruction-set.md §6.2）。两侧同处一个二进制，跨构建抖动
//   （±5-10%，见 lexer-notes §1）不进入差值结论。
//
// 计时纪律（同 lexer_bench）：进程内 best-of-N 取最小，不单发进程计时（同一二进制重复运行的抖动在
// ±1% 内）。本基准的计时单位是「整段 VM 运行」（毫秒级）而非逐 token 的紧循环，故 best-of-N 循环
// 收在一个普通函数里--lexer_bench 的「不抽模板 + lambda 外壳」针对的是外壳开销与测量单位同量级
// （逐 token 循环）的情形，此处不适用。
//
// 复现须用优化构建（无 LTO，理由同 lexer_bench）：
//   cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
//   cmake --build build/rel --target vm_bench -j
//   ./build/rel/bench/vm_bench
// Debug 构建（ASSERT 全开、无内联）下数字成倍偏离，只可对拍行为，程序会自行提示。

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <limits>
#include <memory>

#include "aria.hpp"
#include "compile/Compiler.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

using aria::AriaVM;
using aria::Compiler;
using aria::GC;
using aria::i64;
using aria::kMainEntryName;
using aria::List;
using aria::new_module;
using aria::ObjFunction;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::UPtr;
using aria::usize;
using aria::io::print;
using aria::io::println;
using aria::src::SourceFile;

namespace {

    using Clock = std::chrono::steady_clock;

    // 场景规模：kSize 个元素 x kRounds 轮，各行迭代次数一致方可比 ns/次。场景源一律写不含上界的
    // `0...N`（含上界的 `0..N` 会多走一轮，元素数与轮数都会与 kSize/kRounds 错位）。规模取得偏大是
    // 为了抬高单次试验的绝对耗时（每行 ~0.1-0.25 s），让计时器噪声与调度抖动占比压到千分位。
    constexpr int kSize        = 4096;
    constexpr int kRounds      = 400;
    constexpr i64 kIterations  = static_cast<i64>(kSize) * kRounds;         // 1,638,400
    constexpr i64 kSumToSize   = static_cast<i64>(kSize) * (kSize - 1) / 2; // 0..kSize-1 之和
    constexpr i64 kSumPlusSize = kSumToSize + kSize;                        // step(v) = v + 1 形态
    constexpr int kTrialRuns   = 25;

    // 始终生效的校验（NDEBUG 下 ASSERT 被消除，故自管 BENCH_CHECK）。
#define BENCH_CHECK(cond, msg)                                                            \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            print(stderr, "[bench] CHECK failed: {} ({}:{})\n", msg, __FILE__, __LINE__); \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (false)

    [[nodiscard]] double elapsed_ms(const Clock::time_point begin, const Clock::time_point end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    }

    // 场景 = 编译一次的入口 fn + 其所属 VM 与源文件。VM 不可 move 故 unique_ptr 持有；源保留整场
    // 以免任何回指悬垂（编译期位置烘焙只须活到 compile() 返回，留着零成本）。
    struct Program {
        UPtr<AriaVM>     vm;
        UPtr<SourceFile> source;
        ObjFunction*     fn;
    };

    [[nodiscard]] Program make_program(const StringView name, String content) {
        // 顶层已禁带值 return（入口返回值恒为模块对象）：场景源码包进探针函数（不调用）编译，
        // 再从入口常量池取 __probe__ 直接 run。探针是普通函数，RETURN 通用写回 callee 槽，
        // 其返回值（各场景尾部的 acc）即 run() 返回值。
        auto   vm = std::make_unique<AriaVM>();
        auto&  gc = vm->gc();
        String wrapped{"fun __probe__() {\n"};
        wrapped += content;
        wrapped += "\n}\n";
        auto  source   = std::make_unique<SourceFile>(String{name}, String{name}, std::move(wrapped));
        auto* module   = new_module(gc, name); // StringView 重载：名字经工厂内部 intern 并自守
        auto  compiled = Compiler::compile(gc, *source, module, kMainEntryName);
        BENCH_CHECK(compiled.has_value(), "compile failed");
        ObjFunction* probe = nullptr;
        for (const auto& constant: (*compiled)->unit().constants) {
            if (constant.is_obj() && constant.as_obj()->is<ObjFunction>() &&
                constant.as_obj()->as<ObjFunction>()->name()->view() == "__probe__") {
                probe = constant.as_obj()->as<ObjFunction>();
            }
        }
        BENCH_CHECK(probe != nullptr, "probe fn missing from entry constants");
        return Program{std::move(vm), std::move(source), probe};
    }

    struct Sample {
        double best_ms;
        double spread_pct; // (中位 - 最小) / 最小，行内抖动自述
        double ns_per_iter;
        double allocs_per_iter; // 确定性：GC 累计分配次数增量 / 迭代数
        i64    value;
    };

    // 每次试验都是整段 VM 运行：run() 自带前后清场，同一 fn 可重复跑；探针函数局部 var 每轮重建，
    // 故各轮状态互不影响（不预热，取最小即已排除首轮冷态）。
    //
    // 分配读数取**最后一次**试验（首轮还在填 intern 池，会比稳定态多算一批串对象）；另断言最后两轮
    // 的分配增量几乎相同 -- 序列确定，若两轮差异超出千分之一即说明场景状态相关，读数不可比。
    [[nodiscard]] Sample measure(const Program& program) {
        double       best = std::numeric_limits<double>::max();
        List<double> samples;
        samples.reserve(kTrialRuns);
        usize allocs      = 0;
        usize prev_allocs = 0;
        i64   value       = 0;
        for (int t = 0; t < kTrialRuns; ++t) {
            const usize before = program.vm->gc().allocation_count();
            const auto  begin  = Clock::now();
            auto        out    = program.vm->run(program.fn);
            const auto  end    = Clock::now();
            BENCH_CHECK(out.has_value(), "run failed");
            value = out->as_int();
            best  = std::min(best, elapsed_ms(begin, end));
            samples.push_back(elapsed_ms(begin, end));
            prev_allocs = allocs;
            allocs      = program.vm->gc().allocation_count() - before;
        }
        // 末两轮分配数应几乎相同（含 GC 阈值漂移的场景可能有 1-2 个对象的尾差，容差取迭代数的千分之一）。
        const auto alloc_slack = static_cast<usize>(kIterations / 1000);
        BENCH_CHECK(allocs > prev_allocs ? allocs - prev_allocs <= alloc_slack : prev_allocs - allocs <= alloc_slack,
                    "allocation count unstable between last two trials");
        std::sort(samples.begin(), samples.end());
        const double median = samples[samples.size() / 2];
        const double n      = static_cast<double>(kIterations);
        return Sample{best, (median - best) / best * 100.0, best * 1e6 / n, static_cast<double>(allocs) / n, value};
    }

    void print_row(const StringView label, const Sample& sample, const StringView note) {
        println("{:<26} {:>8.2f} ms ±{:>4.1f}% {:>7.1f} ns/次 {:>8.3f} 分配/次  {}", label, sample.best_ms,
                sample.spread_pct, sample.ns_per_iter, sample.allocs_per_iter, note);
    }

    // 单形态场景：编译 + best-of-N + 结果校验（结果值即「跑的确是预期工作」的凭据）。
    void bench_alone(const StringView label, String source, const i64 expected, const StringView note) {
        const auto program = make_program("bench", std::move(source));
        const auto sample  = measure(program);
        BENCH_CHECK(sample.value == expected, "unexpected result");
        print_row(label, sample, note);
    }

    // 差值行：时间差与确定性差值（分配/次）并列--后者零抖动，跨构建对照时以它为准绳。
    void print_delta_row(const StringView label, const Sample& a, const Sample& b, const StringView note) {
        const double delta_ns     = a.ns_per_iter - b.ns_per_iter;
        const double delta_allocs = a.allocs_per_iter - b.allocs_per_iter;
        println("{:<26} {:>8.2f} ms {:>5} {:>7.1f} ns/次 {:>8.3f} 分配/次  {} ({:.1f}%)", label, a.best_ms - b.best_ms,
                "-", delta_ns, delta_allocs, note, delta_ns / a.ns_per_iter * 100.0);
    }

    // 双形态对照：协议形态 `recv.name(args)` vs 预绑定形态（方法值读到局部再调）。两者算同一结果，
    // 断言相等即证明两形态压的是同一份工作；差值行即不绑定派发的可回收上界，返回给 main 作摘要。
    [[nodiscard]] Pair<Sample, Sample> bench_pair(const StringView protocol_label, String protocol_src,
                                                  const StringView prebound_label, String prebound_src,
                                                  const i64 expected) {
        const auto protocol_program = make_program("bench", std::move(protocol_src));
        const auto prebound_program = make_program("bench", std::move(prebound_src));
        const auto protocol         = measure(protocol_program);
        const auto prebound         = measure(prebound_program);
        BENCH_CHECK(protocol.value == expected && prebound.value == expected, "unexpected result");
        BENCH_CHECK(protocol.value == prebound.value, "两形态结果不一致");
        print_row(protocol_label, protocol, "协议形态");
        print_row(prebound_label, prebound, "预绑定形态（下界）");
        print_delta_row("^ 协议 - 预绑定", protocol, prebound, "差值 = 不绑定派发可回收上界");
        return {protocol, prebound};
    }

    // ---- 源生成 ----

    // 迭代协议对照（list）：协议形态即 forIn 降糖形态（iter 一次、has_next/next 每轮一次）；
    // 预绑定形态显式取迭代器后把两个方法值读到局部（每轮各一次 LOAD_FIELD），循环体零 LOAD_FIELD。
    [[nodiscard]] String make_list_protocol_source() {
        String src;
        src += "var xs = [];\n";
        src += std::format("for (i in 0...{}) {{ xs.push(i); }}\n", kSize);
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += "  for (v in xs) { acc = acc + v; }\n";
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    [[nodiscard]] String make_list_prebound_source() {
        String src;
        src += "var xs = [];\n";
        src += std::format("for (i in 0...{}) {{ xs.push(i); }}\n", kSize);
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += "  var it = xs.iter();\n";
        src += "  var has_next = it.has_next;\n";
        src += "  var next = it.next;\n";
        src += "  while (has_next()) { acc = acc + next(); }\n";
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 迭代协议对照（range）：range 迭代器为无源对象子类，与 list 迭代器同走方法面协议。
    [[nodiscard]] String make_range_protocol_source() {
        String src;
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  for (v in 0...{}) {{ acc = acc + v; }}\n", kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    [[nodiscard]] String make_range_prebound_source() {
        String src;
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var rng = 0...{};\n", kSize);
        src += "  var it = rng.iter();\n";
        src += "  var has_next = it.has_next;\n";
        src += "  var next = it.next;\n";
        src += "  while (has_next()) { acc = acc + next(); }\n";
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 单方法调用对照（string.starts_with）：非迭代协议的一次 `LOAD_FIELD` + `CALL`，方法体不分配；
    // 预绑定形态在轮次循环外取一次方法值。内层用 while 计数以免把 range 迭代协议混进测量。
    [[nodiscard]] String make_method_call_protocol_source() {
        String src;
        src += std::format("var s = \"{}\";\n", String(kSize, 'x'));
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format(
                "  var i = 0;\n  while (i < {}) {{ if (s.starts_with(\"xxx\")) {{ acc = acc + 1; }} i += 1; }}\n",
                kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    [[nodiscard]] String make_method_call_prebound_source() {
        String src;
        src += std::format("var s = \"{}\";\n", String(kSize, 'x'));
        src += "var sw = s.starts_with;\n";
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var i = 0;\n  while (i < {}) {{ if (sw(\"xxx\")) {{ acc = acc + 1; }} i += 1; }}\n",
                           kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 实例方法调用：走不绑定形态（`load_field_unbound`：每次按当前类链解析、零 bound 分配），
    // 成本落在类链查表 + 一次 CALL，与 instance_read 的「每次读现场绑定」形成对照。
    [[nodiscard]] String make_instance_method_source() {
        String src;
        src += "def Counter {\n";
        src += "  init() { this.base = 1; }\n";
        src += "  step(v) { return this.base + v; }\n";
        src += "}\n";
        src += "var c = Counter();\n";
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var i = 0;\n  while (i < {}) {{ acc = acc + c.step(i); i += 1; }}\n", kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 实例方法值读取：每迭代一次 `f = obj.m`。读路径每次现场绑定（方法值是一等值，必须产出
    // bound 对象），故每迭代一次分配 -- 与调用路径的零分配形成对照，是本基准盯住的两条路径之一。
    [[nodiscard]] String make_instance_read_source() {
        String src;
        src += "def Counter {\n";
        src += "  init() { this.n = 0; }\n";
        src += "  step() { return 1; }\n";
        src += "}\n";
        src += "var c = Counter();\n";
        // 循环外先读一次:与预绑定形态结构同态,循环体内「每迭代恰一次 bound 分配」从第 1 轮即成立。
        src += "var f = c.step;\n";
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var i = 0;\n  while (i < {}) {{ f = c.step; acc = acc + 1; i += 1; }}\n", kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 用户类算子重载：同一钩子的两条**取名**路径。钩子形态 `a + b` 走 ADD -> Object::op_add_impl ->
    // ObjInstance 按名（VM 常量串表，零分配零哈希）沿实例 fields/类链取实现 -> call_value；显式按名形态
    // `a.__add__(b)` 的名字由字节码常量池给出，走 PREPARE_METHOD + CALL_METHOD 两段派发，不经运行期取名。
    // 两形态算同一结果，差值行的**符号**即「运行期按名取实现」相对「名字已在手边」的代价方向。
    constexpr i64 kHookOperandSum = 7; // Box(3) + Box(4)
    constexpr i64 kSumHookOperand = kHookOperandSum * kIterations;

    [[nodiscard]] String make_instance_operator_source(const StringView op_expr) {
        String src;
        src += "def Box {\n";
        src += "  init(v) { this.v = v; }\n";
        src += "  __add__(o) { return this.v + o.v; }\n";
        src += "}\n";
        src += "var a = Box(3);\n";
        src += "var b = Box(4);\n";
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var i = 0;\n  while (i < {}) {{ acc = acc + {}; i += 1; }}\n", kSize, op_expr);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 普通函数调用：无 LOAD_FIELD 的调用下界（进帧 + 返回 + 一次 dispatch）。
    [[nodiscard]] String make_plain_call_source() {
        String src;
        src += "fun step(v) { return v + 1; }\n";
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += std::format("  var i = 0;\n  while (i < {}) {{ acc = acc + step(i); i += 1; }}\n", kSize);
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // string 逐码点迭代：产出 1-char string（每迭代一个分配），故只计数不累加（累加串会把分配成本
    // 放大成主导项）；本行量的是「迭代协议 + 逐码点产出」，非纯派发成本。
    [[nodiscard]] String make_string_forin_source() {
        String src;
        src += std::format("var s = \"{}\";\n", String(kSize, 'y'));
        src += "var n = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += "  for (ch in s) { n = n + 1; }\n";
        src += "}\n";
        src += "return n;\n";
        return src;
    }

    // map 迭代：每次产出 [k, v] 二元 list（每迭代一个分配），同 string 行只计数求和键。
    [[nodiscard]] String make_map_forin_source() {
        String src;
        src += "var m = {};\n";
        src += std::format("for (i in 0...{}) {{ m[i] = i; }}\n", kSize);
        src += "var acc = 0;\n";
        src += std::format("for (r in 0...{}) {{\n", kRounds);
        src += "  for (pair in m) { acc = acc + pair[0]; }\n";
        src += "}\n";
        src += "return acc;\n";
        return src;
    }

    // 噪声地板：同一份源编成两个 Program（独立编译、独立 VM）各测一遍--两侧工作完全相同，差值即
    // 「本机本构建下把同一程序当两件事测」的噪声地板。跨构建对照（改动前后）时，小于本行的差值不可信。
    void bench_noise_floor() {
        const auto a = measure(make_program("bench", make_list_protocol_source()));
        const auto b = measure(make_program("bench", make_list_protocol_source()));
        BENCH_CHECK(a.value == b.value, "同一程序两次运行结果不一致");
        BENCH_CHECK(a.allocs_per_iter == b.allocs_per_iter, "同一程序两次运行分配数不一致（场景不确定）");
        print_delta_row("同一程序重复测量", a, b, "噪声地板");
    }

} // namespace

int main() {
    println("VM performance benchmark (best-of-{} = min per row; 每行循环体 {} 次)", kTrialRuns, kIterations);
    println("列义：best 时间 / 行内抖动(中位 - 最小) / ns per 循环体 / GC 分配次数 per 循环体（确定性）");
#ifndef NDEBUG
    println("注意：本构建未定义 NDEBUG（Debug / 未优化），数字只可对拍行为，不可与优化构建比较。");
#endif
    println("");

    println("-- 迭代协议：方法派发对照（list）--");
    const auto list_pair = bench_pair("forin_list (协议)", make_list_protocol_source(), "forin_list (预绑定)",
                                      make_list_prebound_source(), kRounds * kSumToSize);
    println("");
    println("-- 迭代协议：方法派发对照（range）--");
    const auto range_pair = bench_pair("forin_range (协议)", make_range_protocol_source(), "forin_range (预绑定)",
                                       make_range_prebound_source(), kRounds * kSumToSize);
    println("");
    println("-- 单方法调用对照（string.starts_with）--");
    const auto method_pair = bench_pair("starts_with (协议)", make_method_call_protocol_source(),
                                        "starts_with (预绑定)", make_method_call_prebound_source(), kRounds * kSize);
    println("");
    println("-- 噪声地板（同一程序当两件事测；跨构建差值须显著大于本行）--");
    bench_noise_floor();
    println("");
    println("-- 实例成员取用（方法调用 / 方法值读取）--");
    bench_alone("instance_call", make_instance_method_source(), kRounds * kSumPlusSize, "实例方法调用");
    bench_alone("instance_read", make_instance_read_source(), kIterations, "实例方法值读取(m=obj.m)");
    println("");
    println("-- 用户类算子重载（同一钩子的两条取名路径）--");
    {
        const auto hook_program     = make_program("bench", make_instance_operator_source("(a + b)"));
        const auto explicit_program = make_program("bench", make_instance_operator_source("a.__add__(b)"));
        const auto hook             = measure(hook_program);
        const auto explicit_name    = measure(explicit_program);
        BENCH_CHECK(hook.value == explicit_name.value, "两形态结果不一致");
        BENCH_CHECK(hook.value == kSumHookOperand, "unexpected result");
        print_row("instance_operator (钩子)", hook, "ADD -> 按名取实现 -> 调用");
        print_row("instance_operator (显式名)", explicit_name, "名字由常量池给出（PREPARE/CALL_METHOD 两段）");
        print_delta_row("^ 钩子 - 显式名", hook, explicit_name, "差值符号 = 运行期按名取实现的代价方向（负值即更省）");
    }
    println("");
    println("-- 基线行（无对照；派发路径改动前后应保持稳定）--");
    bench_alone("plain_call", make_plain_call_source(), kRounds * kSumPlusSize, "调用下界（无 LOAD_FIELD）");
    bench_alone("forin_string", make_string_forin_source(), kIterations, "逐码点产出 1-char string");
    bench_alone("forin_map", make_map_forin_source(), kRounds * kSumToSize, "逐 pair 产出 [k, v] list");
    println("");

    // 跨构建对照摘要：改动前后 diff 本块即可（协议/预绑定两侧都印，后者是控制组）。
    println("-- 跨构建对照摘要 --");
    const auto summary_row = [](const StringView scene, const Pair<Sample, Sample>& pair) {
        println("{:<14} 协议 {:>6.1f} / 预绑定 {:>6.1f} ns/次 | 分配 {:>7.3f} / {:>7.3f} per 次", scene,
                pair.first.ns_per_iter, pair.second.ns_per_iter, pair.first.allocs_per_iter,
                pair.second.allocs_per_iter);
    };
    summary_row("forin_list", list_pair);
    summary_row("forin_range", range_pair);
    summary_row("starts_with", method_pair);
    println("done.");
    return 0;
}
