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
    EXPECT_EQ(vm.interpret_from_src("assert(1 + 2 * 3 == 7);"), InterpretResult::Ok);
}

// 字符串源：语法错误（缺右操作数）-> CompileError。
TEST(Interpret, StringCompileError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("var x = ;"), InterpretResult::CompileError);
}

// 字符串源：整除零 -> 运行期错误 -> RuntimeError。
TEST(Interpret, StringRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("1 / 0;"), InterpretResult::RuntimeError);
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
    EXPECT_EQ(vm.interpret_from_src("var s = \"a\" + \"b\"; s += \"c\"; assert(s + str(1) == \"abc1\");"),
              InterpretResult::Ok);
}

// 字符串源：String + 非 String -> 运行期 TypeMismatch（无隐式转字符串）-> RuntimeError。
TEST(Interpret, StringPlusNonStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("\"a\" + 1;"), InterpretResult::RuntimeError);
}

// 字符串源：四个比较算子按字节序可用（含空串、前缀、多字节）-> Ok。
TEST(Interpret, StringComparisonOk) {
    AriaVM vm;
    vm.gc().set_stress(true);
    EXPECT_EQ(vm.interpret_from_src("assert(\"a\" < \"b\"); assert(\"b\" >= \"b\");"
                                    "assert(\"\" < \"a\"); assert(\"é\" > \"z\");"),
              InterpretResult::Ok);
}

// 字符串源：String < 非 String -> 运行期 TypeMismatch（定向文案）-> RuntimeError。
TEST(Interpret, StringCompareNonStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("\"a\" < 1;"), InterpretResult::RuntimeError);
}

// 字符串源：非 String < String 同样类型错（左值非对象，仍走数值路径）-> RuntimeError。
TEST(Interpret, NonStringCompareStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("1 < \"a\";"), InterpretResult::RuntimeError);
}

// 字符串源：非 String + String 同样类型错（左值非对象，仍走数值路径）-> RuntimeError。
TEST(Interpret, NonStringPlusStringIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("1 + \"a\";"), InterpretResult::RuntimeError);
}

// 字符串源：读未定义全局 -> 运行期 LOAD_GLOBAL miss 抛 UndefinedVariable -> RuntimeError。
TEST(Interpret, StringRuntimeUndefinedVariableIsRuntimeError) {
    AriaVM vm;
    EXPECT_EQ(vm.interpret_from_src("println(nope);"), InterpretResult::RuntimeError);
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
    const auto path = write_tmp_aria("ok.aria", "assert(7 * 6 == 42);");
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

// 协程挂起期间 stress GC：挂起协程经用户全局变量可达，其值栈中含唯一引用对象（列表）时
// 不被回收；多轮垃圾分配后再 resume，协程内部读到的值完好。
TEST(Interpret, SuspendedCoroutineValuesSurviveStressGc) {
    AriaVM vm;
    vm.gc().set_stress(true);
    const auto src = R"(
        fun body() {
            var inner = [1, 2, 3];
            coroutine.yield(inner.size());
            return inner[1];
        }
        var co = coroutine.create(body);
        assert(coroutine.resume(co) == 3);
        var junk = [];
        var i = 0;
        while (i < 50) {
            junk.push([i]);
            i = i + 1;
        }
        assert(coroutine.resume(co) == 2);
    )";
    EXPECT_EQ(vm.interpret_from_src(src), InterpretResult::Ok);
}

// 两协程交错挂起：各自值栈持唯一引用对象（string 与 list），交错 resume + 垃圾压力下互不干扰。
TEST(Interpret, InterleavedCoroutinesSurviveStressGc) {
    AriaVM vm;
    vm.gc().set_stress(true);
    const auto src = R"(
        fun a_body() {
            var m = "aaa";
            coroutine.yield(m);
            return m.size();
        }
        fun b_body() {
            var m = [9];
            coroutine.yield(m);
            return m[0];
        }
        var ca = coroutine.create(a_body);
        var cb = coroutine.create(b_body);
        assert(coroutine.resume(ca) == "aaa");
        assert(coroutine.resume(cb) == [9]);
        var junk = [];
        var i = 0;
        while (i < 50) {
            junk.push([i]);
            i = i + 1;
        }
        assert(coroutine.resume(ca) == 3);
        assert(coroutine.resume(cb) == 9);
    )";
    EXPECT_EQ(vm.interpret_from_src(src), InterpretResult::Ok);
}
