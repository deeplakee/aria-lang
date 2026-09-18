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
        auto module = new_module(gc, "<test>"); // StringView 重载:名字经工厂内部 intern 并自守
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
        auto  vm       = std::make_unique<AriaVM>();
        auto& gc       = vm->gc();
        auto  module   = new_module(gc, "<test>"); // StringView 重载:名字经工厂内部 intern 并自守
        auto  source   = std::make_unique<SourceFile>("<test>", "<test>", aria::String{src});
        auto  compiled = Compiler::compile(gc, *source, module, aria::kMainEntryName);
        EXPECT_FALSE(compiled.has_value());
        // module 仍由 GC 管理。error 的 SourceLoc 指向 *source，source 经 unique_ptr 存活故可渲染。
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

// var 自引用：声明名在初始化器求值后才登记，init 里的 x 落全局（无则双 miss）-> 运行期
// UndefinedVariable。Error 构造期已把位置烘进自有消息串（含源名 <test>），message() 不依赖
// SourceFile 存活、不应崩溃。
TEST(Compiler, VarSelfRefRuntimeError) {
    auto out = run_source("fun f() { var x = x + 1; return x; } return f();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
    const auto rendered = out.error().message();
    EXPECT_NE(rendered.find("<test>"), std::string::npos);
}

// 编译失败：语法错误 -> 首错 Error（来自 Parser，经 Compiler 取 errors_[0]）。
TEST(Compiler, CompileErrorSyntax) {
    auto fail = compile_fail("var = ");
    EXPECT_FALSE(fail.error().code() == ErrorCode::Ok);
}

// ---- list 方法（push/pop：内置侧方法机制，LOAD_FIELD 恒绑定 + 原生 slots[0]=receiver） ----

// push 追加到末尾、接受任意 Value、返回 nil（变更方法不鼓励链式）。
TEST(Compiler, ListPushAppendsAndReturnsNil) {
    EXPECT_EQ(run_int("var xs = [1, 2]; var r = xs.push(3); if (r == nil) { return xs[2]; } return -1;"), 3);
    EXPECT_EQ(run_int(R"(var xs = [1]; xs.push("ab"); xs.push(nil); xs.push([2]); return len(xs);)"), 4);
}

// pop 移除并返回末元素，长度随之缩减。
TEST(Compiler, ListPopReturnsLastAndShrinks) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; var last = xs.pop(); return last * 10 + len(xs);"), 32);
}

// 元数检查：push 恰 1 参、pop 恰 0 参（文案 builtins 同款）。
TEST(Compiler, ListMethodWrongArity) {
    auto push = run_source("var xs = [1]; return xs.push();");
    ASSERT_FALSE(push.has_value());
    EXPECT_EQ(push.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(push.error().message().find("push expects 1 argument, got 0"), std::string::npos);

    auto pop = run_source("var xs = [1]; return xs.pop(9);");
    ASSERT_FALSE(pop.has_value());
    EXPECT_EQ(pop.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(pop.error().message().find("pop expects no arguments, got 1"), std::string::npos);
}

// pop 空表：IndexOutOfBounds（nil 哨兵不可行 --list 可合法存 nil，fail-fast）。
TEST(Compiler, ListPopEmptyFails) {
    auto out = run_source("var xs = []; return xs.pop();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out.error().message().find("pop from empty list"), std::string::npos);
}

// 恒绑定：取出方法值再调用，仍作用于原 receiver（内置类表条目全为方法）。
TEST(Compiler, ListMethodBoundToReceiver) {
    EXPECT_EQ(run_int("var xs = [1]; var f = xs.push; f(2); return xs[1];"), 2);
}

// 成员 miss：类措辞随协议传播（与实例路径 obj.x 的报错形态一致）。
TEST(Compiler, ListMemberMissFailsWithClassWording) {
    auto out = run_source("var xs = [1]; return xs.foo;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(out.error().message().find("<class List> has no member 'foo'"), std::string::npos);
}

// 成员写入不受支持：store_field 不 override，基类默认即正确行为（不可变成员面）。
TEST(Compiler, ListMemberStoreNotSupported) {
    auto out = run_source("var xs = [1]; xs.foo = 2; return 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(out.error().message().find("type List does not support field access"), std::string::npos);
}

// init 沿链解析到 Object 根的 no-op init：不动槽 0（已是 receiver），调用返回 receiver 自身
// （计划 D1 已知悉接受的小语义毛边，钉住防无声漂移）。
TEST(Compiler, ListInitResolvesToObjectRootNoOp) {
    EXPECT_EQ(run_int("var xs = [1]; if (xs.init() === xs) { return 1; } return 0;"), 1);
}

// 循环内反复取方法（每次现场物化 bound 对象）+ stress GC（run_source 默认开）：
// bound 白色建成即写回原槽根化、方法原生经寄存器组 -> 类链可达。
TEST(Compiler, ListMethodLoopUnderStressGc) {
    EXPECT_EQ(run_int("var xs = [0]; var i = 0; while (i < 60) { xs.push(i); i = i + 1; } return len(xs);"), 61);
}

// ---- forIn 与迭代协议（ObjIterator 子类 / iter / has_next / next / IterationExhausted） ----

// forIn 求和:list 走通内置迭代协议（降糖蓝图:iter/has_next/next 三方法调用）。
TEST(Compiler, ForInSumsList) {
    EXPECT_EQ(run_int("var sum = 0; for (x in [10, 20, 30]) { sum = sum + x; } return sum;"), 60);
}

// 空表零轮,循环从未进体。
TEST(Compiler, ForInEmptyListZeroRounds) { EXPECT_EQ(run_int("var n = 0; for (x in []) { n = n + 1; } return n;"), 0); }

// 嵌套遍历同一 list:两个迭代器游标独立,互不串扰。
TEST(Compiler, ForInNestedSameList) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; var n = 0; for (x in xs) { for (y in xs) { n = n + 1; } } return n;"), 9);
}

// break/continue:continue 跳回 has_next 判断,break 跳出并弹 <iter>（LoopCtx 代弹）。
TEST(Compiler, ForInBreakContinue) {
    EXPECT_EQ(run_int("var s = 0; for (x in [1, 2, 3, 4, 5]) { if (x == 2) { continue; } if (x == 4) { break; } s = s "
                      "+ x; } return s;"),
              4);
}

// 手写迭代:iter/has_next/next 是普通方法,while 驱动与 forIn 同协议。
TEST(Compiler, ManualWhileIteration) {
    EXPECT_EQ(run_int("var it = [5, 6].iter(); var s = 0; while (it.has_next()) { s = s + it.next(); } return s;"), 11);
}

// 协议方法是一等值:取出再调用,恒作用于原 receiver。
TEST(Compiler, IteratorMethodFirstClass) {
    EXPECT_EQ(run_int("var it = [7, 8].iter(); var n = it.next; return n() + n();"), 15);
}

// iter() 每次返回新迭代器（=== 按身份）。
TEST(Compiler, IterReturnsFreshIterator) {
    EXPECT_EQ(run_int("var xs = [1]; if (xs.iter() === xs.iter()) { return 0; } return 1;"), 1);
}

// next 越界抛 IterationExhausted:未捕获走 RuntimeError,消息可核对。
TEST(Compiler, NextExhaustedIsRuntimeError) {
    auto out = run_source("var it = [1].iter(); it.next(); return it.next();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IterationExhausted);
    EXPECT_NE(out.error().message().find("iterator exhausted"), std::string::npos);
}

// IterationExhausted 可 catch（fail-fast 与异常通道合流）。
TEST(Compiler, NextExhaustedCatchable) {
    EXPECT_EQ(run_int("var it = [1].iter(); it.next(); var caught = 0; try { it.next(); } catch (e) { caught = 1; } "
                      "return caught;"),
              1);
}

// 用户类实现协议三方法:与内置 list 同一降糖路径（编译器零魔法,协议对两类来源不可区分）。
TEST(Compiler, UserClassIterableSamePath) {
    EXPECT_EQ(run_int("def Counter {\n"
                      "    init() { this.i = 0; }\n"
                      "    iter() { return this; }\n"
                      "    has_next() { return this.i < 3; }\n"
                      "    next() { var v = this.i; this.i = this.i + 1; return v; }\n"
                      "}\n"
                      "var sum = 0;\n"
                      "for (x in Counter()) { sum = sum + x; }\n"
                      "return sum;"),
              3);
}

// 不可迭代:原语走 LOAD_FIELD 非对象守卫;对象无 iter 方法则类措辞 miss（均 UndefinedProperty,
// NotIterable/IteratorProtocol 两码预留不接线）。
TEST(Compiler, ForInNotIterableFails) {
    auto primitive = run_source("var z = 0; for (x in 5) { z = z + 1; } return 1;");
    ASSERT_FALSE(primitive.has_value());
    EXPECT_EQ(primitive.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(primitive.error().message().find("type Int does not support field access"), std::string::npos);

    auto missing = run_source("def NoIter { init() { } } for (x in NoIter()) { } return 1;");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(missing.error().message().find("<class NoIter> has no member 'iter'"), std::string::npos);
}

// stress GC 下反复 forIn:迭代器对象、<iter> 局部、每轮 bound 物化的根化路径。
TEST(Compiler, ForInUnderStressGc) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; var s = 0; var i = 0; while (i < 20) { for (x in xs) { s = s + x; } i = i + "
                      "1; } return s;"),
              120);
}
