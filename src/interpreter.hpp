#ifndef ARIA_INTERPRETER_HPP
#define ARIA_INTERPRETER_HPP

// 解释器 CLI 入口分发：把命令行参数解析 -> --eval / <file> / REPL，main 仅转调本头。
// 实现收敛在 interpreter.cpp，本头仅暴露公共 API（LineReader + 派发/执行入口），
// 避免把 AriaVM / GC / ObjModule 等内部依赖泄漏给 main.cpp 与测试。
//
// 分发优先级：--help > --version > --eval > --repl > <file> > 默认 REPL（无参即进 REPL）。
// 退出码：0 成功；1 任何错误（CLI 解析失败 / 编译错误 / 运行期错误 / 加载失败）。

#include <functional>

#include "common.hpp"

namespace aria {

    // REPL 行读取器：写入一行到 out 并返回 true；返回 false 表输入结束（EOF / 退出指令）。
    // 交互入口（main）用 isocline 实现，测试用流读取器 lambda 注入。
    using LineReader = std::function<bool(String&)>;

    // CLI 分发核心：解析 argc/argv -> 派发到 eval / file / REPL。返回退出码（0 成功 / 1 错误）。
    // 经 Cli::parse(argc, argv) 重载（零拷贝、跳过 argv[0]）解析。repl_reader 供 REPL 模式逐行读入。
    [[nodiscard]]
    int cli_dispatch(i32 argc, char* argv[], const LineReader& repl_reader);

} // namespace aria

#endif // ARIA_INTERPRETER_HPP