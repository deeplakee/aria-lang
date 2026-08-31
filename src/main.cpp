// aria 解释器入口：解析命令行参数 -> 派发到 --eval / <file> / 交互式 REPL。
// 分发逻辑收敛在 interpreter.hpp 的 cli_dispatch（供测试驱动），本文件仅负责 isocline 行读取器
// 与 argc/argv 透传。退出码：0 成功，1 任何错误（见 cli_dispatch 注释）。
#include "interpreter.hpp"

#include "isocline.h"

using namespace aria;

namespace {

    // isocline 行读取器：提供行编辑 / 历史。读到 EOF（ic_readline 返 nullptr）或
    // exit/quit 指令时返回 false 结束 REPL；非空行写入 out 交 cli_dispatch 执行。
    bool isocline_reader(String& out) {
        char* input = ic_readline("aria"); // isocline 自动在提示符后追加 '>'
        if (input == nullptr) {
            return false; // EOF（Ctrl+D / 管道结束）
        }
        out = input;
        free(input); // isocline 返回的缓冲区由调用方释放
        if (out == "exit" || out == "quit") {
            return false;
        }
        return true;
    }

} // namespace

int main(const int argc, char* argv[]) {
    ic_set_history(nullptr, 500);
    return cli_dispatch(argc, argv, isocline_reader);
}
