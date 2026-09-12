// Compiler/VM 编排层端到端测试：实际 SourceFile -> AriaVM::run(source, module)（编译并执行），
// 辅以 Compiler::compile 直测编译错误路径。验证「源文件到执行结果」端到端可用，覆盖编译失败首错 Error。
#include <gtest/gtest.h>

#include <memory>
#include <string_view>

#include "compile/Compiler.hpp"
#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/source_file.hpp"

using aria::AriaVM;
using aria::Compiler;
using aria::Error;
using aria::ErrorCode;
using aria::i64;
using aria::new_module;
using aria::new_string;
using aria::ObjFunction;
using aria::Result;
using aria::SourceFile;
using aria::Value;

namespace {

    // 持有 AriaVM（进而其 GC）+ 实际 SourceFile + 结果一并返回，使返回值引用的 GC 对象在调用方
    // 检视期间存活--否则辅助函数返回即销毁局部 vm/source -> GC 回收对象悬垂。
    // Error 构造期已把位置烘成自有串、不再指向 SourceFile，故 SourceFile 的存活只关涉 GC 对象
    // （ObjFunction 等）的检视，与 Error 无关；unique_ptr 持堆稳定地址仍为 GC 对象地址稳定所需。
    // 转发 has_value/error 并提供 bool/->/* ，调用点按 Result 惯用法访问（CPP_Naming_Convention
    // 「Optional/Result 用法」：取值 */->，判断隐式 bool，不设 .value()）。
    struct RunResult {
        std::unique_ptr<AriaVM>     vm;
        std::unique_ptr<SourceFile> source;
        Result<Value, Error>        result;
        bool                        has_value() const noexcept { return result.has_value(); }
        explicit                    operator bool() const noexcept { return has_value(); }
        Value*                      operator->() noexcept { return &*result; }
        Value&                      operator*() noexcept { return *result; }
        Error&                      error() noexcept { return result.error(); }
        const Error&                error() const noexcept { return result.error(); }
    };

    // 端到端：构造实际 SourceFile -> AriaVM::run(source, module)（编译并执行）。经 VM 的编译并执行入口
    // （内部 Compiler 编排 + run(ObjFunction*)），非手拼 lex/parse/codegen/run。开 stress GC 锻炼编译期 +
    // 运行期根接线（module 经 CodeGen::compile 内部 make_guard 根化、值栈/帧经 vm_roots tracer 标根）。
    // source 经 unique_ptr 持堆，活到调用方检视完返回值/错误（编译期 Error 的 SourceLoc 指向它）。
    RunResult run_source(std::string_view src) {
        auto  vm = std::make_unique<AriaVM>();
        auto& gc = vm->gc();
        gc.set_stress(true);
        auto mod_name = new_string(gc, "<test>");
        auto guard    = gc.make_guard(mod_name); // 工厂不守入参:name 裸持跨 new_module 的 new_string(cwd)
        auto module   = new_module(gc, mod_name);
        // 实际源文件：调用方构造（测试用 "<test>" 作 name/path，真实入口用文件路径）。堆地址稳定。
        auto source = std::make_unique<SourceFile>("<test>", "<test>", aria::String{src});
        auto result = vm->run(*source, module); // 编译并执行
        return RunResult{std::move(vm), std::move(source), std::move(result)};
    }

    // 编译失败时返回首错 Error（SourceLoc 指向 source，经 unique_ptr 活到检视完，format() 不触悬垂）。
    // Error 无默认构造，故存整个 Result。
    struct CompileFail {
        std::unique_ptr<AriaVM>     vm;
        std::unique_ptr<SourceFile> source;
        Result<ObjFunction*, Error> result;
        Error&                      error() noexcept { return result.error(); }
        const Error&                error() const noexcept { return result.error(); }
    };

    CompileFail compile_fail(std::string_view src) {
        auto     vm       = std::make_unique<AriaVM>();
        auto&    gc       = vm->gc();
        auto     mod_name = new_string(gc, "<test>");
        auto     guard    = gc.make_guard(mod_name);
        auto     module   = new_module(gc, mod_name);
        auto     source   = std::make_unique<SourceFile>("<test>", "<test>", aria::String{src});
        Compiler compiler{gc};
        auto     compiled = compiler.compile(*source, module);
        EXPECT_FALSE(compiled.has_value());
        // guard 释放临时根；module 仍由 GC 管理。error 的 SourceLoc 指向 *source，source 经 unique_ptr 存活故可渲染。
        (void) guard;
        return CompileFail{std::move(vm), std::move(source), std::move(compiled)};
    }

    i64 run_int(std::string_view src) {
        auto out = run_source(src);
        EXPECT_TRUE(out.has_value()) << "expected success";
        return out ? out->as_int() : 0;
    }

} // namespace

// 基础算术：源文件 -> ObjFunction -> 运行得整数（入口 <main> 为函数，顶层 return 合法）。
TEST(Compiler, Arithmetic) {
    EXPECT_EQ(run_int("return 1 + 2 * 3;"), 7);
    EXPECT_EQ(run_int("return (1 + 2) * 3;"), 9);
}

// 函数与递归：经 Compiler 编译后 VM 能调用并返回。
TEST(Compiler, FunctionAndRecursion) {
    EXPECT_EQ(run_int("fun fib(n) { if (n < 2) { return n; } return fib(n - 1) + fib(n - 2); } return fib(10);"), 55);
}

// 全局变量与控制流。
TEST(Compiler, GlobalsAndWhile) {
    EXPECT_EQ(run_int("var x = 0; var i = 1; while (i <= 5) { x = x + i; i = i + 1; } return x;"), 15);
}

// 编译失败：函数体内局部自引用（局部做编译期 init 追踪）-> 首错 UninitializedVariable。
// （顶层 var x = x + 1 是全局自引用，编译期合法、运行期才 UndefinedVariable，故须置于函数体内。）
TEST(Compiler, CompileErrorUninitialized) {
    auto fail = compile_fail("fun f() { var x = x + 1; }");
    ASSERT_EQ(fail.error().code(), ErrorCode::UninitializedVariable);
    // Error 构造期已把位置烘进自有消息串（含源名 <test>），message() 不依赖 SourceFile 存活、不应崩溃。
    const auto rendered = fail.error().message();
    EXPECT_NE(rendered.find("<test>"), std::string::npos);
}

// 编译失败：语法错误 -> 首错 Error（来自 Parser，经 Compiler 取 errors_[0]）。
TEST(Compiler, CompileErrorSyntax) {
    auto fail = compile_fail("var = ");
    EXPECT_FALSE(fail.error().code() == ErrorCode::Ok);
}
