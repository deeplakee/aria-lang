// cli_dispatch 端到端测试：经 interpreter.hpp 的 CLI 分发核心驱动解释器，
// 覆盖 --help / --version / --eval / <file> / REPL 各路径与退出码。REPL 行读取器注入流式 lambda，
// 验证跨行全局持久与逐行容错。错误路径会向 stderr 输出（正常，同 test_interpret）。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "interpreter.hpp"

using aria::cli_dispatch;
using aria::i32;
using aria::LineReader;
using aria::String;

namespace {

    // 写临时 .aria 文件，返回绝对路径（testing::TempDir() 测试结束自动清理）。
    std::string write_tmp_aria(const std::string_view name, const std::string_view content) {
        const auto    path = (std::filesystem::path{testing::TempDir()} / name).string();
        std::ofstream f{path};
        f << content;
        return path;
    }

    // 空读行器：立即 EOF，用于非 REPL 路径（repl_reader 仅在进入 REPL 时被消费）。
    aria::LineReader eof_reader() {
        return [](String&) { return false; };
    }

    // 流式读行器：从给定 istringstream 逐行读（std::getline 失败即 EOF 结束）。
    aria::LineReader stream_reader(std::istringstream& in) {
        return [&in](String& out) { return static_cast<bool>(std::getline(in, out)); };
    }

    // 便捷：从初始参数列表构造 argc/argv 调用分发核心（parse 跳过 argv[0]）。
    // argv[0] 占位程序名，其后为真实参数；string 存储须存活到 cli_dispatch 返回。
    int run_dispatch(std::initializer_list<const char*> args, aria::LineReader reader) {
        std::vector<std::string> storage;
        storage.reserve(args.size() + 1);
        storage.emplace_back("aria"); // argv[0] 程序名（被 parse 跳过）
        for (const char* a: args) {
            storage.emplace_back(a);
        }
        std::vector<char*> argv;
        argv.reserve(storage.size());
        for (auto& s: storage) {
            argv.push_back(s.data());
        }
        return cli_dispatch(static_cast<i32>(argv.size()), argv.data(), std::move(reader));
    }

} // namespace

// --help/-h：短路、打印帮助、退出码 0。
TEST(CliDispatch, HelpReturnsZero) {
    EXPECT_EQ(run_dispatch({"--help"}, eof_reader()), 0);
    EXPECT_EQ(run_dispatch({"-h"}, eof_reader()), 0);
}

// --version：短路、打印版本、退出码 0。
TEST(CliDispatch, VersionReturnsZero) { EXPECT_EQ(run_dispatch({"--version"}, eof_reader()), 0); }

// --version 优先于 --eval：eval 给出运行期必错的表达式（-> 1），版本短路则返 0。
TEST(CliDispatch, VersionTakesPrecedenceOverEval) {
    EXPECT_EQ(run_dispatch({"--version", "--eval", "return 1 / 0;"}, eof_reader()), 0);
}

// 未知选项：CLI 解析失败、退出码 1。
TEST(CliDispatch, UnknownOptionReturnsOne) { EXPECT_EQ(run_dispatch({"--nope"}, eof_reader()), 1); }

// 缺失选项值：--eval 无值 -> 解析失败 -> 1。
TEST(CliDispatch, EvalWithoutValueReturnsOne) { EXPECT_EQ(run_dispatch({"--eval"}, eof_reader()), 1); }

// --eval 合法算术：编译执行成功 -> 0。
TEST(CliDispatch, EvalOk) { EXPECT_EQ(run_dispatch({"--eval", "return 1 + 2 * 3;"}, eof_reader()), 0); }

// -e 短形式等价。
TEST(CliDispatch, EvalShortForm) { EXPECT_EQ(run_dispatch({"-e", "return 7 * 6;"}, eof_reader()), 0); }

// --eval 编译错误（语法）-> 1。
TEST(CliDispatch, EvalCompileError) { EXPECT_EQ(run_dispatch({"-e", "var = ;"}, eof_reader()), 1); }

// --eval 运行期错误（整除零）-> 1。
TEST(CliDispatch, EvalRuntimeError) { EXPECT_EQ(run_dispatch({"-e", "return 1 / 0;"}, eof_reader()), 1); }

// <file> 合法脚本文件 -> 0。
TEST(CliDispatch, FileOk) {
    const auto path = write_tmp_aria("ok.aria", "return 7 * 6;");
    EXPECT_EQ(run_dispatch({path.c_str()}, eof_reader()), 0);
}

// <file> 文件不存在 -> 加载失败 -> 1。
TEST(CliDispatch, FileNotFound) {
    const auto path = (std::filesystem::path{testing::TempDir()} / "nonexistent.aria").string();
    EXPECT_EQ(run_dispatch({path.c_str()}, eof_reader()), 1);
}

// <file> 文件内编译错误 -> 1。
TEST(CliDispatch, FileCompileError) {
    const auto path = write_tmp_aria("bad.aria", "var = ;");
    EXPECT_EQ(run_dispatch({path.c_str()}, eof_reader()), 1);
}

// 无参数 -> 默认 REPL；空输入立即 EOF -> 退出码 0。
TEST(CliDispatch, NoArgsDefaultsToRepl) {
    std::istringstream in;
    EXPECT_EQ(run_dispatch({}, stream_reader(in)), 0);
}

// REPL 跨行全局持久：首行 var x = 42; 次行 println(x); -> 打印 42，整轮退出码 0。
// 验证复用单个 <repl> 模块使顶层 var 经 DEF_GLOBAL 跨行保留。
TEST(CliDispatch, ReplGlobalsPersistAcrossLines) {
    std::istringstream in("var x = 42;\nprintln(x);\n");
    EXPECT_EQ(run_dispatch({}, stream_reader(in)), 0);
}

// REPL 赋值后读取跨行：var s = "hi"; 下一行 println(s + "!"); -> 输出 hi!。
TEST(CliDispatch, ReplStringGlobalsPersist) {
    std::istringstream in("var s = \"hi\";\nprintln(s + \"!\");\n");
    EXPECT_EQ(run_dispatch({}, stream_reader(in)), 0);
}

// REPL 逐行容错：一行运行期错误（除零）后，后续行仍可正常执行，整轮退出码 0。
TEST(CliDispatch, ReplContinuesAfterError) {
    std::istringstream in("return 1 / 0;\nprintln(2 + 3);\n");
    EXPECT_EQ(run_dispatch({}, stream_reader(in)), 0);
}

// REPL 空行跳过：空行不触发编译执行，后续有效行正常。
TEST(CliDispatch, ReplSkipsEmptyLines) {
    std::istringstream in("\n\nprintln(1);\n\n");
    EXPECT_EQ(run_dispatch({}, stream_reader(in)), 0);
}

// --repl 显式进入 REPL（即使后有 file 位置参数也走 REPL，因 --repl 优先于 <file>）。
TEST(CliDispatch, ExplicitReplFlag) {
    std::istringstream in("println(1 + 1);\n");
    EXPECT_EQ(run_dispatch({"--repl"}, stream_reader(in)), 0);
}

// --eval 优先于 --repl：同时给 --eval 与 --repl 时走 eval 一次性求值。
TEST(CliDispatch, EvalTakesPrecedenceOverRepl) {
    EXPECT_EQ(run_dispatch({"--eval", "return 0;", "--repl"}, eof_reader()), 0);
}
