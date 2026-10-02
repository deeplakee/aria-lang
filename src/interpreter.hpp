#ifndef ARIA_INTERPRETER_HPP
#define ARIA_INTERPRETER_HPP

// 解释器 CLI 入口分发：命令行参数解析 -> --eval / <file> / REPL。
// 实现收口在 interpreter.cpp，本头仅暴露公共 API，避免把 AriaVM / GC / ObjModule 等内部
// 依赖泄漏给 main.cpp 与测试。

#include <functional>

#include "common.hpp"

namespace aria {

    // REPL 行读取器：写入一行到 out 并返回 true；返回 false 表输入结束。
    using LineReader = std::function<bool(String&)>;

    // CLI 分发核心：解析 argc/argv 并派发 eval / file / REPL；返回退出码（0 成功 / 1 错误）。
    [[nodiscard]]
    int cli_dispatch(i32 argc, char* argv[], const LineReader& repl_reader);

} // namespace aria

#endif // ARIA_INTERPRETER_HPP
