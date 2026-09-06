// aria 解释器入口：解析命令行参数 -> 派发到 --eval / <file> / 交互式 REPL。
// 分发逻辑收敛在 interpreter.cpp 的 cli_dispatch（声明于 interpreter.hpp，供测试驱动），
// 本文件仅负责 isocline 行读取器与 argc/argv 透传。退出码：0 成功，1 任何错误。
#include "interpreter.hpp"

#include "aria.hpp"
#include "isocline.h"

using namespace aria;

namespace {

    // isocline 行读取器：提供行编辑 / 历史。读到 EOF（ic_readline 返 nullptr）或
    // exit/quit 指令时返回 false 结束 REPL；行（含空行）原样写入 out，空行过滤在 run_repl。
    bool isocline_reader(String& out) {
        char* input = ic_readline(kProductName.data()); // 常量字面量后备保证 '\0' 结尾;isocline 自动在提示符后追加 '>'
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
