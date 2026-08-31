#ifndef ARIA_INTERPRETER_HPP
#define ARIA_INTERPRETER_HPP

// 解释器 CLI 入口分发：把命令行参数解析 -> --eval / <file> / REPL，main 仅转调本头。
// 抽出为独立单元（header-only，属 aria_core）以便测试驱动：main.cpp 持 isocline 读行器，
// 测试注入流式读行器，共享同一 cli_dispatch。
//
// 分发优先级：--help > --eval > --repl > <file> > 默认 REPL（无参即进 REPL）。
// 退出码：0 成功；1 任何错误（CLI 解析失败 / 编译错误 / 运行期错误 / 加载失败）。
//
// REPL 行状态跨行持久：复用单个 <repl> 模块逐行 run(SourceFile&, ObjModule&) 编译执行，
// 顶层 var 声明经 DEF_GLOBAL 落入该模块 globals_，跨行保留（对齐 Python 交互式 globals 复用）。
// 模块经 GC 临时根（make_guard）跨行保活，否则 run() 间 GC 会回收未入模块表的孤立模块。

#include <functional>

#include "common.hpp"
#include "error/Error.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/cli.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

namespace aria {

    // REPL 行读取器：写入一行到 out 并返回 true；返回 false 表输入结束（EOF / 退出指令）。
    // 交互入口（main）用 isocline 实现，测试用流读取器 lambda 注入。
    using LineReader = std::function<bool(String&)>;

    // 构建 aria 解释器 CLI 定义（--help/-h 内置、--repl、--eval/-e、<file> 可选位置参数）。
    inline util::Cli build_cli() {
        util::Cli cli{"aria"};
        cli.set_description("aria 脚本语言解释器");
        cli.add_flag("repl", "启动交互式 REPL");
        cli.add_option("eval", "求值源码字符串后退出", "", 'e');
        cli.add_positional("file", "待运行的 .aria 脚本文件", false);
        return cli;
    }

    // REPL 主循环：复用单个 <repl> 模块逐行编译执行，顶层全局跨行持久。
    // 每行错误经 run() 返回 Error -> format() 渲染到 stderr，循环不中断（REPL 容错继续）。
    inline void repl_run(AriaVM& vm, const LineReader& reader) {
        auto& gc     = vm.gc();
        auto  name   = new_string(gc, "<repl>");
        auto  guard  = gc.make_guard(name);  // name 跨 new_module 内部 new_string(cwd)/new_object 保活
        auto  module = new_module(gc, name); // root 缺省 = cwd（不可用时空串兜底）
        guard.push(module);                  // 模块跨行保活：run() 间 GC 不回收 -> globals_ 持久

        String line;
        while (reader(line)) {
            if (line.empty()) {
                continue;
            }
            // SourceFile 就地构造：name/path 仅显示用，content=本行源码；存活到 run 返回，编译期
            // Error 的 SourceLoc 指向它（不跨行复用，逐行独立源文件）。
            SourceFile source{"<repl>", "", line};
            auto       result = vm.run(source, *module);
            if (!result) {
                io::println(stderr, "{}", result.error().format());
            }
        }
    }

    // CLI 分发核心：解析 argc/argv -> 派发到 eval / file / REPL。返回退出码（0 成功 / 1 错误）。
    // 经 Cli::parse(argc, argv) 重载（零拷贝、跳过 argv[0]）解析。repl_reader 供 REPL 模式逐行读入。
    [[nodiscard]]
    inline int cli_dispatch(const i32 argc, char* argv[], const LineReader& repl_reader) {
        const auto cli    = build_cli();
        auto       parsed = cli.parse(argc, argv);
        if (!parsed) {
            io::println(stderr, "aria: {}", parsed.error());
            io::println("{}", cli.help());
            return 1;
        }
        auto& r = parsed.value();
        if (r.has("help")) {
            io::println("{}", cli.help());
            return 0;
        }

        AriaVM vm;
        // --eval：一次性求值字符串后退出。
        if (r.has("eval")) {
            const auto src = r.get("eval").value_or("");
            return vm.interpret(src) == InterpretResult::Ok ? 0 : 1;
        }
        // --repl：显式进入交互式 REPL。
        if (r.has("repl")) {
            repl_run(vm, repl_reader);
            return 0;
        }
        // <file>：运行脚本文件。
        if (auto file = r.get("file")) {
            return vm.interpret_from_path(*file) == InterpretResult::Ok ? 0 : 1;
        }
        // 无参数 -> 默认 REPL。
        repl_run(vm, repl_reader);
        return 0;
    }

} // namespace aria

#endif // ARIA_INTERPRETER_HPP
