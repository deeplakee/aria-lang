// aria 语言脚本级端到端语料 runner：参数化 GTest，语料树内每个 .aria 脚本一个用例。
// 目录分层、命名、golden/.err 规则与当前禁区见 tests/language/README.md。
//
// 判定规则（按脚本相对语料根的路径）：
//   positive/**                 -> interpret_from_path 须 Ok（脚本内以 assert 自检；
//                                  存在同名 .out 时 stdout 逐字节精确比对）。
//   negative/compile_errors/compile_*.aria -> 须 CompileError。
//   negative/runtime_errors/runtime_*.aria -> 须 RuntimeError（存在同名 .err 时，
//                                  stderr 须包含 .err 每行子串）。
// 路径中任何分量名为 lib 的目录整体跳过（09_modules 的辅助模块只经 import 驱动）。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "runtime/AriaVM.hpp"
#include "type.hpp"

#ifndef ARIA_LANG_CORPUS_DIR
    #error "ARIA_LANG_CORPUS_DIR 未定义：应由 tests/CMakeLists.txt 的 target_compile_definitions 注入语料根绝对路径"
#endif

namespace {

    using namespace std::string_view_literals;

    using aria::AriaVM;
    using aria::InterpretResult;
    using aria::List;
    using aria::String;
    using aria::StringView;
    using aria::usize;

    namespace stdfs = std::filesystem;

    // 语料用例类别：与目录结构一一对应，Malformed 用于钉住违反分层/命名约定的脚本。
    enum class CaseKind { Positive, CompileError, RuntimeError, Malformed };

    // 单个语料用例：脚本路径 + 判定所需全部静态信息（collect 期一次备齐，用例体零 IO）。
    struct CorpusCase {
        String       path;           // 脚本绝对路径（喂给 interpret_from_path）
        String       rel_path;       // 相对语料根路径（分类依据；失败消息展示）
        String       name;           // ctest 用例名：rel_path 去 .aria 后缀、/ 换 _
        CaseKind     kind;           // 判定类别
        bool         has_golden;     // positive：存在同名 .out golden（捕获 stdout 比对）
        String       golden;         // .out 全文（逐字节比对）
        bool         has_err;        // runtime 负向：存在同名 .err（捕获 stderr 比对）
        List<String> err_substrings; // .err 每行一个子串，全部须出现在 stderr
    };

    // gtest 参数打印：失败消息里展示相对路径即可定位，不倾倒全文。
    void PrintTo(const CorpusCase& c, std::ostream* os) { *os << c.rel_path; }

    // 读全文（二进制、保留原始字节含末尾换行）；不存在/不可读返 nullopt。
    std::optional<String> read_file_bytes(const stdfs::path& p) {
        std::ifstream f{p, std::ios::binary};
        if (!f.is_open()) {
            return std::nullopt;
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return ss.str();
    }

    // .err 每行一个子串（丢弃行尾 \r 与空行）；子串须全部出现在 stderr 输出中。
    List<String> split_err_lines(const StringView text) {
        auto lines = List<String>{};
        auto start = usize{0};
        while (start <= text.size()) {
            const auto end  = text.find('\n', start);
            const auto len  = (end == StringView::npos ? text.size() : end) - start;
            auto       line = String{text.substr(start, len)};
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (!line.empty()) {
                lines.push_back(std::move(line));
            }
            if (end == StringView::npos) {
                break;
            }
            start = end + 1;
        }
        return lines;
    }

    // 相对路径 -> 判定类别。约定：positive/ 前缀；negative/ 两个子目录内文件名以
    // compile_/runtime_ 前缀显式声明期望类别，其余一律 Malformed（用例体报错钉住）。
    CaseKind classify(const StringView rel) {
        const auto file = rel.substr(rel.find_last_of('/') + 1);
        if (rel.starts_with("positive/")) {
            return CaseKind::Positive;
        }
        if (rel.starts_with("negative/compile_errors/") && file.starts_with("compile_")) {
            return CaseKind::CompileError;
        }
        if (rel.starts_with("negative/runtime_errors/") && file.starts_with("runtime_")) {
            return CaseKind::RuntimeError;
        }
        return CaseKind::Malformed;
    }

    // 相对路径 -> ctest 用例名：去 .aria 后缀、/ 换 _（如 positive/01_lexical/a.aria ->
    // positive_01_lexical_a）。
    String make_name(const StringView rel) {
        auto name = String{rel};
        if (name.ends_with(".aria")) {
            name.resize(name.size() - ".aria"sv.size());
        }
        std::replace(name.begin(), name.end(), '/', '_');
        return name;
    }

    // 发现语料：语料根下递归收集 .aria，跳过任何 lib 目录分量，按相对路径排序保证确定性。
    List<CorpusCase> collect() {
        const auto root = stdfs::path{ARIA_LANG_CORPUS_DIR};

        auto cases = List<CorpusCase>{};
        for (auto it = stdfs::recursive_directory_iterator{root}; it != stdfs::recursive_directory_iterator{}; ++it) {
            const auto& entry = *it;
            if (!entry.is_regular_file() || entry.path().extension() != ".aria") {
                continue;
            }
            auto under_lib = false;
            for (const auto& comp: entry.path().parent_path()) {
                if (comp == "lib") {
                    under_lib = true;
                    break;
                }
            }
            if (under_lib) {
                continue;
            }

            auto c     = CorpusCase{};
            c.path     = entry.path().string();
            c.rel_path = stdfs::relative(entry.path(), root).string();
            c.name     = make_name(c.rel_path);
            c.kind     = classify(c.rel_path);

            // golden/.err 同名旁挂文件（.aria -> .out / .err），collect 期读齐。
            auto side = entry.path();
            side.replace_extension(".out");
            if (auto golden = read_file_bytes(side)) {
                c.has_golden = true;
                c.golden     = std::move(*golden);
            }
            side.replace_extension(".err");
            if (auto err = read_file_bytes(side)) {
                c.has_err        = true;
                c.err_substrings = split_err_lines(*err);
            }
            cases.push_back(std::move(c));
        }

        std::sort(cases.begin(), cases.end(),
                  [](const CorpusCase& a, const CorpusCase& b) { return a.rel_path < b.rel_path; });
        return cases;
    }

    // 语料树健康度检查：非空、无 Malformed、名字无重复（防 CMake 宏指错根/命名撞车静默漏测）。
    void expect_corpus_healthy(const List<CorpusCase>& cases) {
        ASSERT_GT(cases.size(), usize{0}) << "语料树为空：ARIA_LANG_CORPUS_DIR=" << ARIA_LANG_CORPUS_DIR;
        auto names = List<String>{};
        for (const auto& c: cases) {
            EXPECT_NE(c.kind, CaseKind::Malformed)
                    << "语料路径违反分层/命名约定（须 positive/** 或 negative/<子目录>/compile_|runtime_ 前缀）: "
                    << c.rel_path;
            names.push_back(c.name);
        }
        std::sort(names.begin(), names.end());
        const auto dup = std::adjacent_find(names.begin(), names.end());
        EXPECT_EQ(dup, names.end()) << "用例名撞车（相对路径 / 换 _ 后重名）: " << *dup;
    }

    class LanguageCorpusSuite : public testing::TestWithParam<CorpusCase> {};

    // 单脚本判定。stdout/stderr 捕获须在断言前取回（gtest 失败输出走 stdout，会污染捕获流）。
    TEST_P(LanguageCorpusSuite, MatchesExpectedOutcome) {
        const auto& c = GetParam();

        if (c.kind == CaseKind::Malformed) {
            FAIL() << "语料路径违反分层/命名约定: " << c.rel_path;
        }

        // 每用例全新 VM + stress GC：语料兼当 GC 压力安全网（与 test_interpret 同法）。
        auto vm = AriaVM{};
        vm.gc().set_stress(true);

        switch (c.kind) {
            case CaseKind::Positive: {
                if (c.has_golden) {
                    testing::internal::CaptureStdout();
                }
                const auto result   = vm.interpret_from_path(c.path);
                auto       captured = String{};
                if (c.has_golden) {
                    std::fflush(stdout);
                    captured = testing::internal::GetCapturedStdout();
                }
                ASSERT_EQ(result, InterpretResult::Ok)
                        << "期望 Ok 的正向脚本失败: " << c.rel_path << "（错误详情已由 interpret 渲染，见上方输出）";
                if (c.has_golden) {
                    EXPECT_EQ(captured, c.golden)
                            << "stdout 与 golden 不一致: " << c.rel_path << " 对 " << c.rel_path << "(.out)";
                }
                break;
            }
            case CaseKind::CompileError: {
                EXPECT_EQ(vm.interpret_from_path(c.path), InterpretResult::CompileError)
                        << "期望 CompileError: " << c.rel_path;
                break;
            }
            case CaseKind::RuntimeError: {
                if (c.has_err) {
                    testing::internal::CaptureStderr();
                }
                const auto result   = vm.interpret_from_path(c.path);
                auto       captured = String{};
                if (c.has_err) {
                    std::fflush(stderr);
                    captured = testing::internal::GetCapturedStderr();
                }
                ASSERT_EQ(result, InterpretResult::RuntimeError) << "期望 RuntimeError: " << c.rel_path;
                for (const auto& sub: c.err_substrings) {
                    EXPECT_NE(captured.find(sub), String::npos)
                            << ".err 子串未出现在 stderr: \"" << sub << "\"（来自 " << c.rel_path << ".err）";
                }
                break;
            }
            case CaseKind::Malformed:
                break; // 入口已 FAIL
        }
    }

    INSTANTIATE_TEST_SUITE_P(LanguageCorpus, LanguageCorpusSuite, testing::ValuesIn(collect()),
                             [](const testing::TestParamInfo<CorpusCase>& info) { return info.param.name; });

    // 发现机制本身的钉子：collect() 健康则语料树可测，否则上面参数化套件会静默缺用例。
    TEST(LanguageCorpusDiscovery, CorpusTreeIsHealthy) { expect_corpus_healthy(collect()); }

} // namespace
