// 解释器 CLI 入口分发实现：build_cli + run_repl/run_src/run_file/cli_dispatch。
// 抽到 .cpp 以把 AriaVM / GC / ObjModule / SourceFile 等内部依赖收口在此，
// interpreter.hpp 仅暴露 LineReader 与派发/执行入口的声明。

#include "interpreter.hpp"

#include "error/Error.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/cli.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

namespace aria {

    namespace {

        // 构建 aria 解释器 CLI 定义（--help/-h 内置、--repl、--eval/-e、<file> 可选位置参数）。
        util::Cli build_cli() {
            util::Cli cli{"aria"};
            cli.set_description("aria 脚本语言解释器");
            cli.add_flag("repl", "启动交互式 REPL");
            cli.add_option("eval", "求值源码字符串后退出", "", 'e');
            cli.add_positional("file", "待运行的 .aria 脚本文件", false);
            return cli;
        }

        // REPL 行状态跨行持久：复用单个 <repl> 模块逐行 run(SourceFile&, ObjModule&) 编译执行，
        // 顶层 var 声明经 DEF_GLOBAL 落入该模块 globals_，跨行保留（对齐 Python 交互式 globals 复用）。
        // 模块经 GC 临时根（make_guard）跨行保活，否则 run() 间 GC 会回收未入模块表的孤立模块。
        void run_repl(const LineReader& reader) {
            AriaVM vm;
            auto&  gc     = vm.gc();
            auto   module = new_module(gc, "<repl>"); // root 缺省 = cwd（不可用时空串兜底）
            auto   guard  = gc.make_guard(module);

            String line;
            while (reader(line)) {
                if (line.empty()) {
                    continue;
                }
                // SourceFile 就地构造：name/path 仅显示用，content=本行源码；存活到 run 返回，编译期
                // Error 的 SourceLoc 指向它（不跨行复用，逐行独立源文件）。
                SourceFile source{"<repl>", "", line};
                if (auto result = vm.run(source, *module); !result) {
                    io::println(stderr, "{}", result.error().format());
                }
            }
        }

        // 一次性求值源码字符串：自建 VM，interpret_from_src 成功返回 0，否则 1。
        int run_src(const StringView src) {
            AriaVM vm;
            return vm.interpret_from_src(src) == InterpretResult::Ok ? 0 : 1;
        }

        // 运行脚本文件：自建 VM，interpret_from_path 成功返回 0，否则 1。
        int run_file(const StringView path) {
            AriaVM vm;
            return vm.interpret_from_path(path) == InterpretResult::Ok ? 0 : 1;
        }

    } // namespace

    // CLI 分发核心：解析 argc/argv -> 派发到 eval / file / REPL。返回退出码（0 成功 / 1 错误）。
    int cli_dispatch(const i32 argc, char* argv[], const LineReader& repl_reader) {
        const auto cli    = build_cli();
        const auto parsed = cli.parse(argc, argv);
        if (!parsed) {
            io::println(stderr, "aria: {}", parsed.error());
            io::println("{}", cli.help());
            return 1;
        }
        const auto& args = parsed.value();
        if (args.has("help")) {
            io::println("{}", cli.help());
            return 0;
        }

        // --eval：一次性求值字符串后退出。
        if (args.has("eval")) {
            return run_src(args.get("eval").value_or(""));
        }
        // --repl：显式进入交互式 REPL。
        if (args.has("repl")) {
            run_repl(repl_reader);
            return 0;
        }
        // <file>：运行脚本文件。
        if (const auto file = args.get("file")) {
            return run_file(*file);
        }
        // 无参数 -> 默认 REPL。
        run_repl(repl_reader);
        return 0;
    }

} // namespace aria
