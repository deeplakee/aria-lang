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
    EXPECT_EQ(vm.interpret_from_src("return 1 + 2 * 3;"), InterpretResult::Ok);
}

// 字符串源：语法错误（缺右操作数）-> CompileError。
TEST(Interpret, StringCompileError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("var x = ;"), InterpretResult::CompileError);
}

// 字符串源：整除零 -> 运行期错误 -> RuntimeError。
TEST(Interpret, StringRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("return 1 / 0;"), InterpretResult::RuntimeError);
}

// 字符串源：解构赋值目标未声明 -> 运行期 STORE_GLOBAL miss 抛 UndefinedVariable -> RuntimeError。
TEST(Interpret, StringDestructureAssignmentToUndeclaredNameIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("[a, b] = [1, 2];"), InterpretResult::RuntimeError);
}

// 字符串源：两侧 String 的 + 拼接（含复合赋值 += 与 str() 显式转换）-> Ok。
TEST(Interpret, StringConcatOk) {
    AriaVM vm;
    vm.gc().set_stress(true);
    EXPECT_EQ(vm.interpret_from_src("var s = \"a\" + \"b\"; s += \"c\"; return s + str(1);"), InterpretResult::Ok);
}

// 字符串源：String + 非 String -> 运行期 TypeMismatch（无隐式转字符串）-> RuntimeError。
TEST(Interpret, StringPlusNonStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("return \"a\" + 1;"), InterpretResult::RuntimeError);
}

// 字符串源：非 String + String 同样类型错（左值非对象，仍走数值路径）-> RuntimeError。
TEST(Interpret, NonStringPlusStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("return 1 + \"a\";"), InterpretResult::RuntimeError);
}

// 字符串源：读未定义全局 -> 运行期 LOAD_GLOBAL miss 抛 UndefinedVariable -> RuntimeError。
TEST(Interpret, StringRuntimeUndefinedVariableIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("print nope;"), InterpretResult::RuntimeError);
}

// 字符串源：给未声明名赋值 -> 运行期 STORE_GLOBAL miss 抛 UndefinedVariable（赋值不隐式创建）->
// RuntimeError。
TEST(Interpret, StringRuntimeUndeclaredAssignmentIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("nope = 1;"), InterpretResult::RuntimeError);
}

// 路径源：被导入模块编译期错误在主模块执行期的 IMPORT 站点浮现（经异常通道传播、可被 try/catch
// 捕获）-> RuntimeError。
TEST(Interpret, PathImportedModuleCompileErrorIsRuntimeError) {
    std::filesystem::create_directories(std::filesystem::path{testing::TempDir()} / "imported_ce");
    const auto main_path = write_tmp_aria("imported_ce/main.aria", "import \"./helper\" as H;");
    write_tmp_aria("imported_ce/helper.aria", "var = 5;"); // 语法错:var 后期望标识符
    AriaVM vm;
    vm.set_source_roots({});
    EXPECT_EQ(vm.interpret_from_path(main_path), InterpretResult::RuntimeError);
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
