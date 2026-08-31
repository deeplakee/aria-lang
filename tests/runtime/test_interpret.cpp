// AriaVM::interpret / interpret_from_path 测试：编译并执行入口返回 InterpretResult 枚举。
// 错误由方法内部渲染到 stderr（测试触发错误时会有 stderr 输出，正常），此处只断言结果类别。
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "runtime/AriaVM.hpp"

using aria::AriaVM;
using aria::InterpretResult;

namespace {

    // 写临时 .aria 文件，返回其绝对路径。testing::TempDir() 测试结束自动清理。
    std::string write_tmp_aria(const std::string_view name, const std::string_view content) {
        const auto    path = (std::filesystem::path{testing::TempDir()} / name).string();
        std::ofstream f{path};
        f << content;
        return path;
    }

} // namespace

// 字符串源：合法算术 -> Ok。
TEST(Interpret, StringOk) {
    AriaVM vm;
    vm.gc().set_stress(true);
    EXPECT_EQ(vm.interpret("return 1 + 2 * 3;"), InterpretResult::Ok);
}

// 字符串源：函数体内局部自引用 -> 编译期 UninitializedVariable -> CompileError。
TEST(Interpret, StringCompileError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret("fun f() { var x = x + 1; }"), InterpretResult::CompileError);
}

// 字符串源：整除零 -> 运行期错误 -> RuntimeError。
TEST(Interpret, StringRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret("return 1 / 0;"), InterpretResult::RuntimeError);
}

// 字符串源：未实现特性（list 字面量）-> 编译期 NotImplemented（现归 Semantic 类）-> CompileError。
// 回归测试：此前 NotImplemented 归 Internal，被 interpret_run 映射为 RuntimeError，误导为运行期错误。
TEST(Interpret, StringNotImplementedIsCompileError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret("print [1, 2, 3];"), InterpretResult::CompileError);
}

// 路径源：合法文件 -> Ok。
TEST(Interpret, PathOk) {
    const auto path = write_tmp_aria("ok.aria", "return 7 * 6;");
    AriaVM     vm;
    vm.gc().set_stress(true);
    EXPECT_EQ(vm.interpret_from_path(path), InterpretResult::Ok);
}

// 路径源：文件不存在 -> LoadError。
TEST(Interpret, PathLoadError) {
    AriaVM     vm;
    const auto path = (std::filesystem::path{testing::TempDir()} / "nonexistent.aria").string();
    EXPECT_EQ(vm.interpret_from_path(path), InterpretResult::LoadError);
}

// 路径源：目录而非文件 -> LoadError（from_path 读盘失败）。
TEST(Interpret, PathDirectoryIsLoadError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_path(testing::TempDir()), InterpretResult::LoadError);
}

// 路径源：文件内编译错误 -> CompileError。
TEST(Interpret, PathCompileError) {
    const auto path = write_tmp_aria("bad.aria", "var = ;");
    AriaVM     vm;
    EXPECT_EQ(vm.interpret_from_path(path), InterpretResult::CompileError);
}
