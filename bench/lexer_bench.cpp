// bench/lexer_bench.cpp
//
// 词法层性能基准：词法吞吐（四种源形态）、端到端编译、位置派生原语、utf8 解码微基准。
// 这些数字与解读归档在 .claude/reference/compile/lexer-notes.md，本程序是它们的可复现入口--
// 改 Lexer / SourceLoc / utf8::decode_one 前后各跑一次对照，把新数字回写文档。
//
// 四种源形态各压不同的成本（规模与 lexer-notes §2 的 MB / token 列对应，改规模须同步文档）：
//   normal   常规代码（多行，注释/字符串/数字混杂）-- 贴近真实源码，压主循环的分派成本
//   numeric  单行 "1,1,1,…"（约每字节一个 token）-- 放大逐 token 的小分派成本；真实代码无此形态
//   cjk      多字节为主（标识符/字符串/注释）-- 把成本压到 utf8::decode_one
//   string   长字符串字面量为主（含多字节与转义）-- 压字符串扫描路径
//   string_huge 同 string 形态、规模更大 -- 压超大字面量路径（词法期记账 + 消费端解码直写铸串）
//
// 端到端编译（Lexer -> Parser -> CodeGen）另量一遍：位置派生的收益要整条链路一起看。
// 位置派生原语（locate / line_at 的行表缓存）单列一节：它们是「位置只存偏移」这一取舍的依据。
// utf8 解码微基准供将来的解码重构对照（lexer-notes §5）--decode_one 的消费者含 Lexer、
// 行表构建与字符串码点遍历等多族,Lexer 只是其一。
//
// 计时纪律（两条都是踩过的坑，见 lexer-notes §1）：
//   - 进程内 best-of-N 取最小，不要单发进程计时（单发易吃相邻工作的干扰,实测数字见 lexer-notes）。
//   - 每节的计时循环写死在原地，不抽公共的「best-of-N 模板 + lambda」外壳：同一二进制内对照，
//     外壳本身就会让同一段词法变慢（实测幅度见 lexer-notes），足以掩盖要量的差异。
//     同理，跨二进制比绝对值要先确认代码布局变了多少。
//
// 复现 lexer-notes 的数字须用优化构建（文档各表统一在 -O2 -DNDEBUG、无 LTO 下测得）：
//   cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release -DARIA_ENABLE_LTO=OFF -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG"
//   cmake --build build/rel --target lexer_bench -j
//   ./build/rel/bench/lexer_bench
// 两处都有量级：Debug 构建（build/ 默认）下 ASSERT 全开、无内联，数字成倍偏离，只可用于对拍行为；
// Release 默认开 LTO，跨 TU 内联让词法再快约 15%（同一命令去掉 -DARIA_ENABLE_LTO=OFF 即可对照,
// 各形态实测幅度见 lexer-notes），故文档各表统一以无 LTO 为基线。

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <format>
#include <limits>

#include "aria.hpp"
#include "compile/Compiler.hpp"
#include "compile/Lexer.hpp"
#include "object/ObjModule.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"
#include "util/utf8.hpp"

namespace utf8 = aria::utf8;

using aria::Compiler;
using aria::GC;
using aria::kMainEntryName;
using aria::Lexer;
using aria::List;
using aria::String;
using aria::u32;
using aria::u64;
using aria::usize;
using aria::io::print;
using aria::io::println;
using aria::src::SourceFile;

namespace {
    using Clock = std::chrono::steady_clock;

    // 源规模（与 lexer-notes §2 的 MB / token 列对应）
    constexpr int kNormalBlocks     = 20'000;  // 3 行/块 -> 约 2.80 MB、840k token
    constexpr int kNumericTokens    = 400'000; // "1," x n -> 800 KB、800k token
    constexpr int kCjkBlocks        = 20'000;  // 2 行/块 -> 约 2.94 MB、340k token
    constexpr int kStringBlocks     = 2'250;   // 约 2.7 MB
    constexpr int kStringHugeBlocks = 5'200;   // 约 6.0 MB（超大字面量形态）
    constexpr int kCompileBlocks    = 5'000;   // 4 行/块 -> 约 0.94 MB
    constexpr int kPositionBlocks   = 25'000;  // 同形态 -> 约 4.75 MB、100k 行

    constexpr int kLexTrials     = 30;
    constexpr int kCompileTrials = 9;
    constexpr int kPosTrials     = 5;
    constexpr int kDecodeTrials  = 5;

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

    [[nodiscard]] SourceFile make_source_file(String content) {
        return SourceFile{String{"bench.aria"}, String{"bench.aria"}, std::move(content)};
    }

    // ---- 源生成 ----

    // 常规代码：行短而多，注释/字符串/浮点/十六进制/中文注释混杂。
    [[nodiscard]] String make_normal_source(const int blocks) {
        String src;
        for (int i = 0; i < blocks; ++i) {
            src += std::format("var name_{} = 1.5e2 + 0x1F; // comment 中文注释\n", i);
            src += std::format("fun f{}(a, b) {{ return a + b * 2 - 1; }}\n", i);
            src += "if (a == b && c || d) { println(\"s\" + 't'); }\n";
        }
        return src;
    }

    // 单行密集：无注释与标识符，只压「每字节一个 token」的小分派。
    [[nodiscard]] String make_numeric_source(const int tokens) {
        String src;
        src.reserve(static_cast<usize>(tokens) * 2);
        for (int i = 0; i < tokens; ++i) {
            src += "1,";
        }
        return src;
    }

    // 多字节密集：标识符/字符串/注释均以 CJK 为主。
    [[nodiscard]] String make_cjk_source(const int blocks) {
        String src;
        for (int i = 0; i < blocks; ++i) {
            src += std::format("变量名称编号{} = 中文字符串常量 + 另一个变量; // 中文注释内容\n", i);
            src += std::format("函数名{}(参数甲, 参数乙) {{ 返回 参数甲; }}\n", i);
        }
        return src;
    }

    // 字符串密集：长字符串字面量（多字节 + 转义）为主。
    [[nodiscard]] String make_string_source(const int blocks) {
        String src;
        for (int i = 0; i < blocks; ++i) {
            src += std::format("var 文本_{} = \"", i);
            for (int k = 0; k < 40; ++k) {
                src += "中文字符串内容abcdefg";
            }
            src += "\\n\\t 结尾\"; // 注释\n";
            src += std::format("var 短_{} = 'x';\n", i);
        }
        return src;
    }

    // 宽行形态：4 行/块（含 for 与数组下标），端到端编译与位置派生共用。
    [[nodiscard]] String make_wide_source(const int blocks) {
        String src;
        for (int i = 0; i < blocks; ++i) {
            src += std::format("var name_{} = 1.5e2 + 0x1F; // comment 中文注释\n", i);
            src += std::format("fun f{}(a, b) {{ return a + b * 2 - 1; }}\n", i);
            src += "if (a == b && c || d) { println(\"s\" + 't'); }\n";
            src += "for (var j = 0; j < 10; ++j) { total += arr[j]; }\n";
        }
        return src;
    }

    // ---- 词法吞吐 ----

    // 一次 tokenize 计一次；SourceFile 在计时外构造，token 流每次重建
    // （与真实调用一致：tokenize 内部一次性构造、无复用）。
    void bench_lex(const char* shape, String content) {
        auto        source = make_source_file(std::move(content));
        const usize bytes  = source.content().size();
        usize       tokens = 0;
        double      best   = std::numeric_limits<double>::max();
        for (int t = 0; t < kLexTrials; ++t) {
            const auto begin  = Clock::now();
            auto       result = Lexer::tokenize(source);
            const auto end    = Clock::now();
            BENCH_CHECK(result.has_value(), "tokenize failed");
            tokens = result->tokens.size();
            best   = std::min(best, elapsed_ms(begin, end));
        }
        const double mb    = static_cast<double>(bytes) / 1e6;
        const double mbs   = mb / (best / 1e3);
        const double ns_tk = best * 1e6 / static_cast<double>(tokens);
        println("{:<8} {:>9.2f} MB {:>9} tok {:>9.3f} ms {:>8.1f} MB/s {:>7.1f} ns/tok", shape, mb, tokens, best, mbs,
                ns_tk);
    }

    // ---- 端到端编译 ----

    // GC 与模块构造在计时外（每次试验重建，与 VM 的真实调用同形）。
    void bench_compile() {
        auto        source = make_source_file(make_wide_source(kCompileBlocks));
        const usize bytes  = source.content().size();
        double      best   = std::numeric_limits<double>::max();
        for (int t = 0; t < kCompileTrials; ++t) {
            GC         gc;
            auto*      module   = new_module(gc, "bench");
            const auto begin    = Clock::now();
            auto       compiled = Compiler::compile(gc, source, module, kMainEntryName);
            const auto end      = Clock::now();
            BENCH_CHECK(compiled.has_value(), "compile failed");
            best = std::min(best, elapsed_ms(begin, end));
        }
        println("{:.2f} MB -> {:.2f} ms (best-of-{})", static_cast<double>(bytes) / 1e6, best, kCompileTrials);
    }

    // ---- 位置派生原语 ----

    // 访问序列取 token 起点偏移：源序即 AST 遍历序（CodeGen 求行号的实际形态），
    // 随机序是行表缓存的最坏形态。
    void bench_position() {
        auto       source  = make_source_file(make_wide_source(kPositionBlocks));
        const auto content = source.content();
        const u32  bytes   = static_cast<u32>(content.size());

        List<u32> offsets;
        {
            auto tokens = Lexer::tokenize(source);
            BENCH_CHECK(tokens.has_value(), "tokenize failed");
            offsets.reserve(tokens->tokens.size());
            for (const auto& token: tokens->tokens) {
                offsets.push_back(token.lexeme().empty() ? bytes
                                                         : static_cast<u32>(token.lexeme().data() - content.data()));
            }
        }

        volatile u64 sink = 0;
        const auto   row  = [&](const char* name, const double best) {
            println("{:<30} {:>9.3f} ms {:>9.2f} ns/次", name, best, best * 1e6 / static_cast<double>(offsets.size()));
        };

        // 源序：行表缓存命中。
        {
            double best = std::numeric_limits<double>::max();
            for (int t = 0; t < kPosTrials; ++t) {
                u64        acc   = 0;
                const auto begin = Clock::now();
                for (const u32 offset: offsets) {
                    acc += source.line_at(offset);
                }
                const auto end = Clock::now();
                sink           = acc;
                best           = std::min(best, elapsed_ms(begin, end));
            }
            row("line_at(offset) 源序（缓存命中）", best);
        }

        // 随机序：每次二分都未命中行缓存。
        {
            double best = std::numeric_limits<double>::max();
            for (int t = 0; t < kPosTrials; ++t) {
                const usize n     = offsets.size();
                u64         acc   = 0;
                const auto  begin = Clock::now();
                for (usize i = 0; i < n; ++i) {
                    acc += source.line_at(offsets[(i * 7919) % n]);
                }
                const auto end = Clock::now();
                sink           = acc;
                best           = std::min(best, elapsed_ms(begin, end));
            }
            row("line_at(offset) 随机序（缓存失效）", best);
        }

        // 冷路径全解析：行号 + 行内码点列（只被错误渲染与测试调用）。
        {
            double best = std::numeric_limits<double>::max();
            for (int t = 0; t < kPosTrials; ++t) {
                u64        acc   = 0;
                const auto begin = Clock::now();
                for (const u32 offset: offsets) {
                    const auto line_col = source.locate(offset);
                    acc += line_col.line + line_col.col;
                }
                const auto end = Clock::now();
                sink           = acc;
                best           = std::min(best, elapsed_ms(begin, end));
            }
            row("locate(offset) 全解析（含码点列）", best);
        }

        const double avg_line = static_cast<double>(bytes) / static_cast<double>(source.line_count());
        println("{:.2f} MB / {} 行 / {} token；平均行长 {:.1f} B--locate 的列部分即行内 [行首, offset) 数码点",
                static_cast<double>(bytes) / 1e6, source.line_count(), offsets.size(), avg_line);
    }

    // ---- utf8 解码微基准 ----

    // 紧循环逐码点解码：每次解码串行依赖游标，会高估它在真实语流里的边际成本，只作纵向对照。
    // 码点累加进 checksum：否则解码结果无人消费，循环会被约简成「按长度跳过」而非真的解码。
    void bench_utf8_decode() {
        String ascii;
        for (int i = 0; i < 5'000; ++i) {
            ascii += std::format("var name_{} = 1.5e2 + 0x1F; // comment line\n", i);
            ascii += std::format("fun f{}(a, b) {{ return a + b * 2 - 1; }}\n", i);
        }
        String cjk;
        for (int i = 0; i < 8'000; ++i) {
            cjk += std::format("变量名称编号{} = 中文字符串常量; // 中文注释内容\n", i);
        }

        const auto decode_row = [](const char* shape, const String& text) {
            usize  codepoints = 0;
            u64    checksum   = 0;
            double best       = std::numeric_limits<double>::max();
            for (int t = 0; t < kDecodeTrials; ++t) {
                u32        pos   = 0;
                usize      n     = 0;
                u64        sum   = 0;
                const auto begin = Clock::now();
                while (pos < text.size()) {
                    const auto [cp, len] = utf8::decode_one(text, pos);
                    BENCH_CHECK(len > 0, "decode_one consumed 0 bytes");
                    sum += cp;
                    pos += len;
                    ++n;
                }
                const auto end = Clock::now();
                checksum       = sum;
                codepoints     = n;
                best           = std::min(best, elapsed_ms(begin, end));
            }
            (void) checksum;
            const double mb  = static_cast<double>(text.size()) / 1e6;
            const double mbs = mb / (best / 1e3);
            println("{:<6} decode_one {:>8.3f} ms {:>7.1f} MB/s {:>6.2f} ns/码点 ({} 码点)", shape, best, mbs,
                    best * 1e6 / static_cast<double>(codepoints), codepoints);
        };

        const auto valid_row = [](const char* shape, const String& text) {
            double best = std::numeric_limits<double>::max();
            for (int t = 0; t < kDecodeTrials; ++t) {
                const auto begin = Clock::now();
                const bool ok    = utf8::is_valid(text);
                const auto end   = Clock::now();
                BENCH_CHECK(ok, "generated source is not valid UTF-8");
                best = std::min(best, elapsed_ms(begin, end));
            }
            println("{:<6} is_valid   {:>8.3f} ms {:>7.1f} MB/s", shape, best,
                    (static_cast<double>(text.size()) / 1e6) / (best / 1e3));
        };

        decode_row("ASCII", ascii);
        decode_row("CJK", cjk);
        valid_row("ASCII", ascii);
        valid_row("CJK", cjk);
    }

} // namespace

int main() {
    println("Lexer performance benchmark (best-of-N = min per row)");
    println("");
    println("-- 词法吞吐 --");
    println("{:<12} {:>12} {:>13} {:>12} {:>11} {:>11}", "shape", "bytes", "tokens", "best ms", "MB/s", "ns/tok");
    bench_lex("normal", make_normal_source(kNormalBlocks));
    bench_lex("numeric", make_numeric_source(kNumericTokens));
    bench_lex("cjk", make_cjk_source(kCjkBlocks));
    bench_lex("string", make_string_source(kStringBlocks));
    bench_lex("string_huge", make_string_source(kStringHugeBlocks));
    println("");
    println("-- 端到端编译（Lexer -> Parser -> CodeGen）--");
    bench_compile();
    println("");
    println("-- 位置派生原语 --");
    bench_position();
    println("");
    println("-- utf8 解码原语 --");
    bench_utf8_decode();
    println("");
    println("done.");
    return 0;
}
