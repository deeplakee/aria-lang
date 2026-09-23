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
    EXPECT_EQ(run_int(R"(var xs = [1]; xs.push("ab"); xs.push(nil); xs.push([2]); return xs.size();)"), 4);
}

// pop 移除并返回末元素，长度随之缩减。
TEST(Compiler, ListPopReturnsLastAndShrinks) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; var last = xs.pop(); return last * 10 + xs.size();"), 32);
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
// （已知悉接受的小语义毛边，钉住防无声漂移）。
TEST(Compiler, ListInitResolvesToObjectRootNoOp) {
    EXPECT_EQ(run_int("var xs = [1]; if (xs.init() === xs) { return 1; } return 0;"), 1);
}

// 循环内反复取方法（每次现场物化 bound 对象）+ stress GC（run_source 默认开）：
// bound 白色建成即写回原槽根化、方法原生经寄存器组 -> 类链可达。
TEST(Compiler, ListMethodLoopUnderStressGc) {
    EXPECT_EQ(run_int("var xs = [0]; var i = 0; while (i < 60) { xs.push(i); i = i + 1; } return xs.size();"), 61);
}

// ---- list 方法面补全（insert/remove/remove_at/clear/sort/reverse/find/contains/size/is_empty） ----

// remove_at(i)：按位置移除并返回；负数从尾计数（与下标读写同语义）。
TEST(Compiler, ListRemoveAt) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; return xs.remove_at(0) * 10 + xs.size();"), 12);
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; return xs.remove_at(-1) * 10 + xs.size();"), 32);
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; return xs.remove_at(1) * 10 + xs[0] + xs[1];"), 24);
}

// remove_at 越界/非整数：静态文案（与切片「slice index out of range」同款，不带键值）。
TEST(Compiler, ListRemoveAtFails) {
    auto out = run_source("var xs = [1]; return xs.remove_at(5);");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out.error().message().find("remove_at index out of range"), std::string::npos);

    auto bad = run_source("var xs = [1]; return xs.remove_at(\"a\");");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(bad.error().message().find("remove_at index must be an integer"), std::string::npos);
}

// remove(x)：移除**全部** == 命中元素，命中 true / 未命中 false（不报错，miss 走返回值与 find/contains 同族）；
// nil/嵌套容器按内容可移除；只要移一处用 find + remove_at 组合。
TEST(Compiler, ListRemove) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; if (xs.remove(2) && xs.size() == 2) { return xs[0] + xs[1]; } return -1;"),
              4);
    EXPECT_EQ(run_int("var xs = [1, 2, 1, 1]; xs.remove(1); if (xs == [2]) { return 1; } return 0;"), 1); // 移全、保序
    EXPECT_EQ(run_int("var xs = [1]; if (!xs.remove(9) && xs.size() == 1) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var xs = [1, nil, nil]; if (xs.remove(nil) && xs == [1]) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var xs = [[1], 2]; if (xs.remove([1]) && xs == [2]) { return 1; } return 0;"), 1);
}

// size/is_empty：元素数与空表谓词。
TEST(Compiler, ListSizeAndIsEmpty) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; return xs.size();"), 3);
    EXPECT_EQ(run_int("var xs = []; return xs.size();"), 0);
    EXPECT_EQ(run_int("var xs = [1]; if (!xs.is_empty() && [].is_empty()) { return 1; } return 0;"), 1);
}

// insert：位置语义（i 之前插入），i == 元素数即追加；负数指「该下标元素之前」。
TEST(Compiler, ListInsert) {
    EXPECT_EQ(run_int("var xs = [1, 3]; xs.insert(1, 2); return xs[0] * 100 + xs[1] * 10 + xs[2];"), 123);
    EXPECT_EQ(run_int("var xs = [1, 3]; xs.insert(0, 9); return xs[0];"), 9);
    EXPECT_EQ(run_int("var xs = [1, 3]; xs.insert(2, 9); return xs.size() * 10 + xs[2];"), 39);
    EXPECT_EQ(run_int("var xs = [1, 3]; xs.insert(-1, 9); return xs[1];"), 9);
    EXPECT_EQ(run_int("var xs = []; xs.insert(0, 7); return xs[0];"), 7);
}

// insert 越界：合法域 [-(size), size]，静态文案（与切片同款，不带键值）。
TEST(Compiler, ListInsertOutOfRangeFails) {
    auto high = run_source("var xs = [1]; xs.insert(5, 0); return 1;");
    ASSERT_FALSE(high.has_value());
    EXPECT_EQ(high.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(high.error().message().find("insert index out of range"), std::string::npos);

    auto low = run_source("var xs = [1]; xs.insert(-5, 0); return 1;");
    ASSERT_FALSE(low.has_value());
    EXPECT_EQ(low.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(low.error().message().find("insert index out of range"), std::string::npos);
}

// clear：清空返 nil，长度归零；别名（共享可变状态）同见。
TEST(Compiler, ListClear) {
    EXPECT_EQ(run_int(R"(var xs = [1, 2]; var alias = xs; var r = xs.clear();
        if (r == nil && xs.size() == 0 && alias == []) { return 1; } return 0;)"),
              1);
}

// sort 数值域：就地升序返 nil；int/f64 混合同域（升 f64 比较）；空表 no-op。
TEST(Compiler, ListSortNumbers) {
    EXPECT_EQ(run_int("var xs = [3, 1, 2]; var r = xs.sort(); if (r == nil) { return xs[0] * 100 + xs[1] * 10 + xs[2]; "
                      "} return -1;"),
              123);
    EXPECT_EQ(run_int("var xs = [3, 1.5, 2]; xs.sort(); if (xs[0] == 1.5) { return xs[1] * 10 + xs[2]; } return -1;"),
              23);
    EXPECT_EQ(run_int("var xs = []; if (xs.sort() == nil) { return xs.size(); } return -1;"), 0);
}

// sort 字符串域：无符号字节序（与比较算子同源，大写 < 小写）；别名可见就地变更。
TEST(Compiler, ListSortStrings) {
    EXPECT_EQ(run_int(R"(var ws = ["pear", "apple", "Banana"]; var alias = ws; ws.sort();
        if (alias === ws && ws[0] == "Banana" && ws[1] == "apple" && ws[2] == "pear") { return 1; } return 0;)"),
              1);
}

// sort 域外：数值与字符串混居 / 不可比元素（nil），TypeMismatch 报两类类型。
TEST(Compiler, ListSortMixedFails) {
    auto mixed = run_source("var xs = [1, \"a\"]; xs.sort(); return 1;");
    ASSERT_FALSE(mixed.has_value());
    EXPECT_EQ(mixed.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(mixed.error().message().find("sort requires all numbers or all strings, got Int and String"),
              std::string::npos);

    auto incomparable = run_source("var xs = [nil]; xs.sort(); return 1;");
    ASSERT_FALSE(incomparable.has_value());
    EXPECT_EQ(incomparable.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(incomparable.error().message().find("sort requires all numbers or all strings, got Nil"),
              std::string::npos);
}

// reverse：就地整段反转返 nil。
TEST(Compiler, ListReverse) {
    EXPECT_EQ(run_int("var xs = [1, 2, 3]; var r = xs.reverse(); if (r == nil) { return xs[0] * 100 + xs[1] * 10 + "
                      "xs[2]; } return -1;"),
              321);
    EXPECT_EQ(run_int("var xs = []; xs.reverse(); return xs.size();"), 0);
}

// find/contains：value_equal（== 内容递归）判定；find 未命中 nil（下标永不为 nil；
// 有负下标故 -1 是合法下标，miss 时 xs[find(x)] 会静默取末元素--Python str.find 的坑，Ruby 返 nil 同款）。
TEST(Compiler, ListFindAndContains) {
    EXPECT_EQ(run_int("var xs = [1, nil, \"a\", [2]]; return xs.find(nil) * 1000 + xs.find(\"a\") * 100 + xs.find([2]) "
                      "* 10;"),
              1230); // 命中下标 1, 2, 3
    EXPECT_EQ(run_int("var xs = [1, nil]; if (xs.contains(nil) && !xs.contains(2)) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var xs = [9]; if (xs.find(1) == nil) { return 1; } return 0;"), 1); // miss 返 nil
    EXPECT_EQ(run_int("var xs = []; if (xs.find(1) == nil && !xs.contains(1)) { return 1; } return 0;"), 1);
}

// ---- 负下标（从尾计数：-1 末元素、-len 首元素；归一化后越界 fail-fast 报原始键值） ----

// 读对称:xs[-1] = 末元素、xs[-len] = 首元素。
TEST(Compiler, ListNegativeIndexReads) {
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; return xs[-1];"), 30);
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; return xs[-3];"), 10);
}

// 写对称:xs[-1] = v 覆写末槽;复合赋值走同一归一化(locator 读改写单链)。
TEST(Compiler, ListNegativeIndexWrites) {
    EXPECT_EQ(run_int("var xs = [10, 20]; xs[-1] = 99; return xs[1];"), 99);
    EXPECT_EQ(run_int("var xs = [10, 20]; xs[-1] += 5; return xs[-1];"), 25);
}

// 越界:归一化后仍负(< -len)即报错,文案报用户写的原始负值。
TEST(Compiler, ListNegativeIndexBoundsFail) {
    auto out = run_source("var xs = [1, 2]; return xs[-3];");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out.error().message().find("list index -3 out of range"), std::string::npos);
}

// ---- 切片（range 作下标键：含否上界/无上界/负端点/倒序产出/只读） ----

// 切片产出新 list:闭区间/半开/无上界/负端点/倒序;size 与 forIn 消费切片结果。
TEST(Compiler, ListSliceReads) {
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40, 50]; var s = xs[1..3]; return s[0] * 100 + s[1] * 10 + s[2];"), 2340);
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40]; return xs[1...3].size();"), 2);
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; return xs[-2..].size();"), 2);
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; return xs[2...2].size();"), 0);
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; var t = 0; for (x in xs[-2..]) { t = t + x; } return t;"), 50);
    // 正起点配负终点(原始端点递减、归一化后正序):方向按归一化端点判,不误报倒序。
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40, 50]; return xs[1..-1].size();"), 4);
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40, 50]; return xs[1..-2][0] * 10 + xs[1..-2][1];"), 230);
    // 倒序 range 作下标:切片按倒序产出(方向判据与 range 迭代同一套)。
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40, 50]; var t = 0; for (x in xs[3..1]) { t = t * 10 + x; } return t;"),
              4320);
    EXPECT_EQ(run_int("var xs = [10, 20, 30]; return xs[2..0][0];"), 30);
    EXPECT_EQ(run_int("var xs = [10, 20, 30, 40, 50]; return xs[-1...0].size();"), 4);
}

// 切片错误面:端点越界 fail-fast、切片写落整数键统一文案。
TEST(Compiler, ListSliceFails) {
    auto out_of_range = run_source("var xs = [1, 2, 3]; return xs[0..10];");
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out_of_range.error().message().find("slice range 0..10 out of range"), std::string::npos);

    auto store = run_source("var xs = [1, 2, 3]; xs[0..2] = [9]; return 0;");
    ASSERT_FALSE(store.has_value());
    EXPECT_EQ(store.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(store.error().message().find("list index must be an integer, got Range"), std::string::npos);
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

// ---- map 字面量与下标（MAKE_MAP / ObjMap 协议 / KeyError / 迭代产出 [k,v] pair） ----

// 字面量构造与任意键下标读;值为任意表达式(运行时求值)。
TEST(Compiler, MapLiteralAndSubscriptRead) {
    EXPECT_EQ(run_int("var m = {\"a\": 1, \"b\": 2}; return m[\"a\"] * 10 + m[\"b\"];"), 12);
    EXPECT_EQ(run_int("var k = \"b\"; var m = {k: 5}; return m[k];"), 5); // 键为任意表达式
    EXPECT_EQ(run_int("var m = {1: [10, 20]}; return m[1][1];"), 20);     // 嵌套容器值
}

// 下标写:未命中新增键、已命中覆写,均恒成功。
TEST(Compiler, MapSubscriptWriteUpserts) {
    EXPECT_EQ(run_int("var m = {}; m[\"a\"] = 1; m[\"a\"] = 2; m[\"b\"] = 3; return m.size() * 10 + m[\"a\"];"), 22);
}

// miss 读:KeyError(运行期,键入文案)。
TEST(Compiler, MapReadMissFailsKeyError) {
    auto out = run_source("var m = {\"a\": 1}; return m[\"nope\"];");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::KeyError);
    EXPECT_NE(out.error().message().find("map key not found"), std::string::npos);
}

// 字面量重复键:后键胜(set 命中原槽覆写,Python dict 同款,零特判)。
TEST(Compiler, MapLiteralDuplicateKeyLastWins) {
    EXPECT_EQ(run_int("var m = {\"a\": 1, \"a\": 2}; return m[\"a\"];"), 2);
    EXPECT_EQ(run_int("return {\"a\": 1, \"a\": 2}.size();"), 1);
}

// size:Map 返键值对数(与 string/list 并列)。
TEST(Compiler, MapLenReturnsEntryCount) {
    EXPECT_EQ(run_int("return {}.size();"), 0);
    EXPECT_EQ(run_int("return {1: 10, 2: 20, 3: 30}.size();"), 3);
}

// 相等 ==:按内容(插入序无关),!= 取反;=== 恒指针。
TEST(Compiler, MapEqualityIsContent) {
    EXPECT_EQ(run_int("var a = {1: 10, 2: 20}; var b = {2: 20, 1: 10}; if (a == b) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var a = {1: 10}; var b = {1: 11}; if (a != b) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var a = {1: 10}; if (a === a) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("var a = {1: 10}; var b = {1: 10}; if (a === b) { return 0; } return 1;"), 1);
}

// forIn 遍历 map:循环变量拿到 [k, v] 二元 list(产出顺序 unspecified,不依赖)。
TEST(Compiler, ForInMapYieldsKeyValuePairs) {
    EXPECT_EQ(run_int("var m = {\"a\": 1, \"b\": 2}; var sum = 0; for (pair in m) { sum = sum + pair[1]; } "
                      "return sum;"),
              3);
    EXPECT_EQ(run_int("var m = {\"x\": 10}; var n = 0; for (pair in m) { if (pair.size() == 2 && pair[0] == \"x\" && "
                      "pair[1] == 10) { n = 1; } } return n;"),
              1);
}

// map 成员 miss:类措辞随协议传播(与 list 同文案形);成员写不受支持。
TEST(Compiler, MapMemberMissAndStoreNotSupported) {
    auto miss = run_source("var m = {}; return m.foo;");
    ASSERT_FALSE(miss.has_value());
    EXPECT_EQ(miss.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(miss.error().message().find("<class Map> has no member 'foo'"), std::string::npos);

    auto store = run_source("var m = {}; m.foo = 2; return 1;");
    ASSERT_FALSE(store.has_value());
    EXPECT_EQ(store.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(store.error().message().find("type Map does not support field access"), std::string::npos);
}

// stress GC 下 map 字面量反复构造 + forIn:键值 peek 在栈跨 new_map、pair 铸造的根化路径。
TEST(Compiler, MapLiteralAndForInUnderStressGc) {
    EXPECT_EQ(run_int("var s = 0; var i = 0; while (i < 20) { for (pair in {\"a\": 1, \"b\": 2}) { s = s + "
                      "pair[1]; } i = i + 1; } return s;"),
              60);
}

// ---- range(区间对象:MAKE_RANGE 发射 + 迭代协议,.. 含上界 / ... 不含) ----

// forIn 求和:含上界 0..5 产出 0..5 六值,不含上界 0...5 产出 0..4 五值。
TEST(Compiler, RangeForInSums) {
    EXPECT_EQ(run_int("var s = 0; for (i in 0..5) { s = s + i; } return s;"), 15);
    EXPECT_EQ(run_int("var s = 0; for (i in 0...5) { s = s + i; } return s;"), 10);
}

// 空区间零迭代:仅 low==high 且不含上界(5...5)首问即 false,循环体零轮。
TEST(Compiler, RangeForInEmptyZeroRounds) {
    EXPECT_EQ(run_int("var n = 0; for (i in 5...5) { n = n + 1; } return n;"), 0);
}

// 倒序:low>high 方向推断为递减,含上界 10..1 产出 10→1(求和 55),不含上界 10...1 产出
// 10→2(求和 54);5..3 三轮末值 3。
TEST(Compiler, RangeForInReversedSums) {
    EXPECT_EQ(run_int("var s = 0; for (i in 10..1) { s = s + i; } return s;"), 55);
    EXPECT_EQ(run_int("var s = 0; for (i in 10...1) { s = s + i; } return s;"), 54);
    EXPECT_EQ(run_int("var n = 0; var last = 0; for (i in 5..3) { n = n + 1; last = i; } return n * 10 + last;"), 33);
}

// 单值区间:5..5 恰一轮、循环变量取 5(含上界的 lo==hi)。
TEST(Compiler, RangeForInSingleValue) {
    EXPECT_EQ(run_int("var n = 0; var last = 0; for (i in 5..5) { n = n + 1; last = i; } return n * 10 + last;"), 15);
}

// 端点是运行期值:全局变量端点、算术表达式端点(优先级 range 低于比较,端点吃满 term)。
TEST(Compiler, RangeComputedBounds) {
    EXPECT_EQ(run_int("var lo = 2; var hi = 6; var s = 0; for (i in lo...hi) { s = s + i; } return s;"), 14);
    EXPECT_EQ(run_int("var s = 0; for (i in 1 + 1..2 * 3) { s = s + i; } return s;"), 20);
    EXPECT_EQ(run_int("var s = 0; for (i in -2..2) { s = s + i; } return s;"), 0);
}

// range 作值:绑定变量后可反复迭代(每次 iter 新铸迭代器,游标互不串扰)。
TEST(Compiler, RangeAsValueReiterated) {
    EXPECT_EQ(run_int("var r = 2..4; var s = 0; for (i in r) { s = s + i; } for (i in r) { s = s + i; } return s;"),
              18);
}

// 相等 ==:按内容(端点与含否上界全等);=== 恒指针。
TEST(Compiler, RangeEqualityIsContent) {
    EXPECT_EQ(run_int("if (0..10 == 0..10) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("if (0..10 == 0...10) { return 0; } return 1;"), 1);
    EXPECT_EQ(run_int("if (0..10 === 0..10) { return 0; } return 1;"), 1);
}

// 非整数端点:TypeMismatch,双值 debug 形文案(执行体就地烘焙)。
TEST(Compiler, RangeNonIntBoundsTypeMismatch) {
    auto out = run_source("return 1.5..10;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(out.error().message().find("range bounds must be integers"), std::string::npos);
}

// 无上界开区间:绑定后 forIn + break 自理边界;from.. 与 from... 同义(equals 相等、
// str 渲染 from..)。
TEST(Compiler, RangeUnboundedBindsAndBreaks) {
    EXPECT_EQ(run_int("var r = 0..; var n = 0; for (i in r) { if (i >= 5) { break; } n = n + 1; } return n;"), 5);
    EXPECT_EQ(run_int("var n = 0; for (i in 3...) { if (i > 5) { break; } n = n * 10 + i; } return n;"), 345);
}

// 无上界拼写同义:0.. == 0...;与有界端点(含 0)不相等;str 渲染 0..。
TEST(Compiler, RangeUnboundedSpellingEquivalent) {
    EXPECT_EQ(run_int("if (0.. == 0...) { return 1; } return 0;"), 1);
    EXPECT_EQ(run_int("if (0.. == 0..0) { return 0; } return 1;"), 1);
    EXPECT_EQ(run_int(R"(if (str(0..) == "0..") { return 1; } return 0;)"), 1);
}

// stress GC 下 range 反复构造 + forIn:端点 peek 在栈跨 new_range、迭代器白色建成的发布路径。
TEST(Compiler, RangeForInUnderStressGc) {
    EXPECT_EQ(run_int("var s = 0; var i = 0; while (i < 20) { for (j in 0..9) { s = s + j; } i = i + 1; } return s;"),
              900);
}

// ---- varargs(...rest:list 载体,call_closure 打包多余实参) ----

// 多余实参按序收集进 rest(list;下标可断言)。
TEST(Compiler, VarargsCollectsExtras) {
    EXPECT_EQ(
            run_int("fun f(a, b, ...rest) { return rest[0] * 100 + rest[1] * 10 + rest[2]; } return f(1, 2, 3, 4, 5);"),
            345);
}

// 无多余实参:rest 为空 list(恒 list 非 nil);size 观察。
TEST(Compiler, VarargsEmptyRest) {
    EXPECT_EQ(run_int("fun f(a, ...rest) { return a * 10 + rest.size(); } return f(7);"), 70);
}

// 默认参数与 varargs 共存矩阵:全传 / 只传必传(缺省走默认 + 空 rest)/ 恰传固定数。
TEST(Compiler, VarargsWithDefaults) {
    EXPECT_EQ(run_int("fun f(a, b = 10, ...rest) { return a + b + rest.size(); } return f(1, 2, 3, 4, 5);"), 6);
    EXPECT_EQ(run_int("fun f(a, b = 10, ...rest) { return a + b + rest.size(); } return f(1);"), 11);
    EXPECT_EQ(run_int("fun f(a, b = 10, ...rest) { return a + b + rest.size(); } return f(1, 2);"), 3);
}

// 纯 varargs(零固定参数):零参与多参皆合法。
TEST(Compiler, VarargsPureRest) {
    EXPECT_EQ(run_int("fun g(...xs) { return xs.size(); } return g() * 10 + g(1, 2, 3);"), 3);
}

// 元数下界仍守(必传不足报 WrongArity 至少式文案);上界取消(8 参照常)。
TEST(Compiler, VarargsArityBound) {
    auto out = run_source("fun f(a, ...rest) { return a; } return f();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(out.error().message().find("expects at least 1 args, got 0"), std::string::npos);

    EXPECT_EQ(run_int("fun f(a, ...rest) { return rest.size(); } return f(1, 2, 3, 4, 5, 6, 7, 8);"), 7);
}

// rest 每次调用新铸:对它 push 不外泄、两次调用互不相干。
TEST(Compiler, VarargsFreshListPerCall) {
    EXPECT_EQ(run_int("fun f(...xs) { xs.push(99); return xs.size(); } var a = f(1); var b = f(1); return a * 10 + b;"),
              22);
}

// rest 是普通局部槽,可被闭包捕获。
TEST(Compiler, VarargsCapturedByClosure) {
    EXPECT_EQ(run_int("fun f(...xs) { var c = fun() { return xs.size(); }; return c() + xs[0]; } return f(5, 6);"), 7);
}

// 方法帧同构适用(槽 0 = this 后照常收集)。
TEST(Compiler, VarargsMethodFrame) {
    EXPECT_EQ(run_int("def Box { init(a, ...rest) { this.a = a; this.n = rest.size(); } } var b = Box(1, 2, 3); return "
                      "b.a * 10 + b.n;"),
              12);
}

// stress GC 下反复打包:多余实参「栈即根」跨 new_list、白色 list 即写回栈的根化路径。
TEST(Compiler, VarargsUnderStressGc) {
    EXPECT_EQ(run_int("fun f(...xs) { var s = 0; for (x in xs) { s = s + x; } return s; } var t = 0; var i = 0; "
                      "while (i < 20) { t = t + f(i, i + 1, i + 2); i = i + 1; } return t;"),
              630);
}

// ---- string 方法面（字节下标/码点迭代/11 方法；下标域 = 字节） ----

// upper/lower:ASCII 逐字节转换,非字母字节原样。
TEST(Compiler, StringUpperLowerAscii) {
    EXPECT_EQ(run_int(R"(if ("abc".upper() == "ABC") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("aBc1".upper() == "ABC1") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("XYZ".lower() == "xyz") { return 1; } return 0;)"), 1);
}

// trim:去首尾 ASCII 空白,全空白返空串。
TEST(Compiler, StringTrimAsciiWhitespace) {
    EXPECT_EQ(run_int(R"(if ("  hi ".trim() == "hi") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("\t\n hi \r\n".trim() == "hi") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("   ".trim() == "") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("a b".trim() == "a b") { return 1; } return 0;)"), 1); // 中间空白不动
}

// split:保留空段、空串输入切出 [""],空分隔符 EmptyPattern。
TEST(Compiler, StringSplitKeepsEmptySegments) {
    EXPECT_EQ(run_int(R"(var parts = "a,,b".split(","); if (parts.size() == 3 && parts[0] == "a" && parts[1] == "" && )"
                      R"(parts[2] == "b") { return 1; } return 0;)"),
              1);
    EXPECT_EQ(run_int(R"(var one = "".split(","); if (one.size() == 1 && one[0] == "") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("x=y=z".split("=")[1] == "y") { return 1; } return 0;)"), 1);

    auto empty = run_source(R"(return "ab".split("");)");
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::EmptyPattern);
    EXPECT_NE(empty.error().message().find("split separator must not be empty"), std::string::npos);
}

// split 的 receiver 若为临时值(不进常量池,仅 intern 弱根),切割循环里的分配会回收它--
// 内容须在覆写 slots[0] 前拷离 GC 堆。字面量 receiver 经常量池强根,故既有用例不覆盖此形态。
TEST(Compiler, StringSplitTemporaryReceiverSurvivesStressGc) {
    EXPECT_EQ(run_int(R"(var xs = ("aaaa," + "bbbbbbbbbbbbbbbb").split(","); )"
                      R"(if (xs.size() == 2 && xs[0] == "aaaa") { return 1; } return 0;)"),
              1);
    EXPECT_EQ(run_int(R"(var xs = ("aaaa," + "bbbbbbbbbbbbbbbb").split(","); )"
                      R"(if (xs[1] == "bbbbbbbbbbbbbbbb") { return 1; } return 0;)"),
              1);
}

// find:首现字节下标,未命中 nil。
TEST(Compiler, StringFindReturnsByteIndexOrNil) {
    EXPECT_EQ(run_int(R"(return "hello".find("llo");)"), 2);
    EXPECT_EQ(run_int(R"(if ("hello".find("x") == nil) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(return "aaa".find("a");)"), 0);
}

// replace:全部替换,空匹配串 EmptyPattern。
TEST(Compiler, StringReplaceAll) {
    EXPECT_EQ(run_int(R"(if ("aaa".replace("a", "bb") == "bbbbbb") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("abc".replace("x", "y") == "abc") { return 1; } return 0;)"), 1);

    auto empty = run_source(R"(return "ab".replace("", "x");)");
    ASSERT_FALSE(empty.has_value());
    EXPECT_EQ(empty.error().code(), ErrorCode::EmptyPattern);
    EXPECT_NE(empty.error().message().find("replace pattern must not be empty"), std::string::npos);
}

// substring:1/2 参双形态,字节区间 [start, end);越界(含负数)IndexOutOfBounds。
TEST(Compiler, StringSubstringRangeChecked) {
    EXPECT_EQ(run_int(R"(if ("hello".substring(1) == "ello") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello".substring(1, 3) == "el") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello".substring(2, 2) == "") { return 1; } return 0;)"), 1);

    auto out_of_range = run_source(R"(return "hello".substring(3, 9);)");
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().code(), ErrorCode::IndexOutOfBounds);

    auto negative = run_source(R"(return "hello".substring(-1);)");
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().code(), ErrorCode::IndexOutOfBounds);
}

// substring 非整数参数:报错须报**违规的那个**参数(两参形态下首个参数违规时报的是它,不是
// 合法的第二个)。
TEST(Compiler, StringSubstringReportsOffendingArgument) {
    auto first_bad = run_source(R"(return "hello".substring("x", 1);)");
    ASSERT_FALSE(first_bad.has_value());
    EXPECT_EQ(first_bad.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(first_bad.error().message().find("got String"), std::string::npos);

    auto second_bad = run_source(R"(return "hello".substring(1, "x");)");
    ASSERT_FALSE(second_bad.has_value());
    EXPECT_EQ(second_bad.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(second_bad.error().message().find("got String"), std::string::npos);
}

// starts_with/ends_with:字节前后缀,空串前缀恒真。
TEST(Compiler, StringStartsAndEndsWith) {
    EXPECT_EQ(run_int(R"(if ("hello".starts_with("he") && !"hello".starts_with("el")) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello".ends_with("lo") && !"hello".ends_with("el")) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("".starts_with("") && "hi".starts_with("")) { return 1; } return 0;)"), 1);
}

// codepoint_at:码点序号索引(区别于字节下标 s[i]) --héllo 的 codepoint_at(1) 是 é 的
// 码点值,而非中间字节;越界 IndexOutOfBounds。
TEST(Compiler, StringCodepointAtIndexesByCodepoint) {
    EXPECT_EQ(run_int(R"(return "héllo".codepoint_at(1);)"), 0xE9);
    EXPECT_EQ(run_int(R"(return "abc".codepoint_at(2);)"), 'c');

    auto out_of_range = run_source(R"(return "héllo".codepoint_at(5);)");
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().code(), ErrorCode::IndexOutOfBounds);
}

// s[i]:字节域,产出单字节 1-char string;越界 IndexOutOfBounds。多字节中间字节的取值
// 语义钉在对象级(ObjString.LoadIndexMidSequenceByteYieldsItself,字面量无 \x 转义故
// 端到端只钉单字节产出)。
TEST(Compiler, StringSubscriptIsByteSemantics) {
    EXPECT_EQ(run_int(R"(if ("hello"[1] == "e") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("héllo".size() == 6 && "héllo"[1].size() == 1) { return 1; } return 0;)"), 1);

    auto out_of_range = run_source(R"(return "hi"[5];)");
    ASSERT_FALSE(out_of_range.has_value());
    EXPECT_EQ(out_of_range.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out_of_range.error().message().find("string index 5 out of range"), std::string::npos);
}

// s[-i]:负下标从尾计数(同 list),-1 末字节、-len 首字节;越界报原始负值。
TEST(Compiler, StringNegativeIndexReads) {
    EXPECT_EQ(run_int(R"(if ("hello"[-1] == "o") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello"[-5] == "h") { return 1; } return 0;)"), 1);

    auto out = run_source(R"(return "hi"[-3];)");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out.error().message().find("string index -3 out of range"), std::string::npos);
}

// string 不可变:下标写恒 TypeMismatch。
TEST(Compiler, StringImmutableStoreFails) {
    auto out = run_source(R"(var s = "hi"; s[0] = "H"; return s;)");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(out.error().message().find("type String does not support subscript assignment"), std::string::npos);
}

// forIn string:逐码点产出 1-char string(文法「string->字符」,D5)。产出验证用赋值 + 比较。
TEST(Compiler, ForInStringYieldsCharStrings) {
    EXPECT_EQ(run_int(R"(var first = ""; var n = 0; for (ch in "abc") { if (n == 0) { first = ch; } n = n + 1; } )"
                      R"(if (first == "a" && n == 3) { return 1; } return 0;)"),
              1);
    // 多字节整步跨过:héllo 字节长 6、码点 5,计数 5(码点域)。
    EXPECT_EQ(run_int(R"(var n = 0; for (ch in "héllo") { n = n + 1; } return n;)"), 5);
    EXPECT_EQ(
            run_int(R"(var all_eq = true; var i = 0; for (ch in "héllo") { if (i == 1 && ch != "é") { all_eq = false; } )"
                    R"(i = i + 1; } if (all_eq) { return 1; } return 0;)"),
            1);
}

// join:list 方法,元素宽松经 format_value 转 string;空 list 返空串;空 sep 粘合。
TEST(Compiler, StringJoinOnListReceiver) {
    EXPECT_EQ(run_int(R"(if (["a", "b", "c"].join("-") == "a-b-c") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if (["a", "b"].join("") == "ab") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ([].join(",") == "") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ([1, "x", nil].join(",") == "1,x,nil") { return 1; } return 0;)"), 1); // 宽松转 string
}

// 成员 miss:类措辞随协议传播(与 list/map 同文案形)。
TEST(Compiler, StringMemberMissFailsWithClassWording) {
    auto out = run_source(R"(return "hi".nope;)");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
    EXPECT_NE(out.error().message().find("<class String> has no member 'nope'"), std::string::npos);
}

// 迭代器一等性:it.next 取出方法值调用;循环取方法 stress GC(bound 物化根化路径)。
TEST(Compiler, StringBuiltinsUnderStressGc) {
    EXPECT_EQ(run_int(R"(var it = "abc".iter(); var n1 = it.next; if (n1() == "a" && it.has_next()) { return 1; } )"
                      R"(return 0;)"),
              1);
    EXPECT_EQ(run_int("var s = 0; var i = 0; while (i < 20) { s = s + \"ab\".upper().trim().size(); i = i + 1; } "
                      "return s;"),
              40);
}

// size/is_empty:字节域;码点数走 chars().size()。
TEST(Compiler, StringSizeIsEmpty) {
    EXPECT_EQ(run_int(R"(return "héllo".size();)"), 6); // 字节域:6 字节、5 码点
    EXPECT_EQ(run_int(R"(return "".size();)"), 0);
    EXPECT_EQ(run_int(R"(if ("".is_empty() && !"a".is_empty()) { return 1; } return 0;)"), 1);
}

// contains:按字节子串判定,未命中 false 不报错;空串参数恒真;非 string 参数 TypeMismatch。
TEST(Compiler, StringContainsSubstring) {
    EXPECT_EQ(run_int(R"(if ("hello".contains("ell") && !"hello".contains("xyz")) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello".contains("") && "".contains("")) { return 1; } return 0;)"), 1);

    auto bad = run_source(R"(return "hello".contains(1);)");
    ASSERT_FALSE(bad.has_value());
    EXPECT_EQ(bad.error().code(), ErrorCode::TypeMismatch);
    EXPECT_NE(bad.error().message().find("contains argument must be a string, got Int"), std::string::npos);
}

// chars:逐码点切 1-char string(与迭代同单位);chars().size() 即码点数;join 回原文;非法字节序列
// 产出替换码点串(同迭代口径)。循环调用 + stress GC 锻炼 list 与逐串铸造的根化路径(guard 承重)。
TEST(Compiler, StringCharsSplitsCodepoints) {
    EXPECT_EQ(run_int(R"(return "héllo".chars().size();)"), 5); // 字节 6、码点 5
    EXPECT_EQ(run_int(R"(if ("héllo".chars()[1] == "é") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("héllo".chars().join("") == "héllo") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(return "".chars().size();)"), 0);
    EXPECT_EQ(run_int(R"(if ("héllo"[1].chars()[0] == "\u{FFFD}") { return 1; } return 0;)"), 1);
    EXPECT_EQ(
            run_int("var n = 0; var i = 0; while (i < 20) { n = n + \"héllo\".chars().size(); i = i + 1; } return n;"),
            100);
}

// to_int/to_float:整串十进制解析,失败(空串/杂字/空白/下划线/进制前缀/越值域)返 nil -- miss 返
// nil 与 find/get 同族;to_float 另收小数形/指数形与 inf/nan(与 str(f64) 的输出往返一致)。
TEST(Compiler, StringToIntAndToFloat) {
    EXPECT_EQ(
            run_int(R"(if ("42".to_int() == 42 && "-7".to_int() == -7 && "+7".to_int() == 7) { return 1; } return 0;)"),
            1);
    EXPECT_EQ(run_int(R"(if ("007".to_int() == 7 && str("42".to_int()) == "42") { return 1; } return 0;)"), 1);
    // 失败一律 nil;小数形只属 to_float,to_int 不收。
    EXPECT_EQ(
            run_int(R"(if ("".to_int() == nil && "abc".to_int() == nil && " 42".to_int() == nil) { return 1; } return 0;)"),
            1);
    EXPECT_EQ(
            run_int(R"(if ("1_000".to_int() == nil && "0x10".to_int() == nil && "3.7".to_int() == nil) { return 1; } return 0;)"),
            1);
    // i48 域:上界内可解析,越界与解析失败同路返 nil(不截断)。
    EXPECT_EQ(run_int(R"(return "140737488355327".to_int();)"), 140737488355327);
    EXPECT_EQ(
            run_int(R"(if ("140737488355328".to_int() == nil && "-140737488355329".to_int() == nil) { return 1; } return 0;)"),
            1);

    EXPECT_EQ(
            run_int(R"(if ("3.5".to_float() == 3.5 && "1e3".to_float() == 1000 && ".5".to_float() == 0.5) { return 1; } return 0;)"),
            1);
    EXPECT_EQ(run_int(R"(if (str("3".to_float()) == "3.0" && str("3.5".to_float()) == "3.5") { return 1; } return 0;)"),
              1);
    EXPECT_EQ(
            run_int(R"(if (str("inf".to_float()) == "inf" && str("nan".to_float()) == "nan") { return 1; } return 0;)"),
            1);
    EXPECT_EQ(
            run_int(R"(if ("".to_float() == nil && "3 ".to_float() == nil && "1e400".to_float() == nil) { return 1; } return 0;)"),
            1);

    auto arity = run_source(R"(return "1".to_int(9);)");
    ASSERT_FALSE(arity.has_value());
    EXPECT_EQ(arity.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(arity.error().message().find("to_int expects no arguments, got 1"), std::string::npos);
}

// 切片:Range 键走字节域切片,与 list 同口径(含/不含上界、负端点从尾计数、无上界后缀、倒序段),
// 越界报 IndexOutOfBounds(与 list 同串)。rest 解构在 string 上经「MAKE_RANGE i.. + LOAD_INDEX」
// 同一机制成立,故一并钉住。
TEST(Compiler, StringRangeSlice) {
    EXPECT_EQ(run_int(R"(if ("hello"[1..3] == "ell" && "hello"[1...3] == "el") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello"[-3..] == "llo" && "hello"[1..-1] == "ello") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello"[3..1] == "lle" && "hello"[0..] == "hello") { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("hello"[5..].size() == 0) { return 1; } return 0;)"), 1); // 末尾之后取剩余 = 空段
    // 倒序是字节域反转:s[i] 取出的单字节串可拼出同一结果(多字节串按字节倒排,非合法 UTF-8)。
    EXPECT_EQ(run_int(R"(if ("héllo"[2..0] == "héllo"[2] + "héllo"[1] + "héllo"[0]) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(var [c, ...r] = "abc"; if (c == "a" && r == "bc") { return 1; } return 0;)"), 1);

    auto out = run_source(R"(return "hello"[1..9];)");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::IndexOutOfBounds);
    EXPECT_NE(out.error().message().find("slice range 1..9 out of range"), std::string::npos);
}

// split():0 参按 ASCII 空白连续段切并丢空段(全空白/空串返 [];不做 Unicode 空白,与 trim 同
// 空白集);1 参形态保留空段不变。
TEST(Compiler, StringSplitOnWhitespace) {
    EXPECT_EQ(run_int(R"(var xs = "  a  b\tc ".split(); )"
                      R"(if (xs.size() == 3 && xs[0] == "a" && xs[1] == "b" && xs[2] == "c") { return 1; } return 0;)"),
              1);
    EXPECT_EQ(run_int(R"(if ("   ".split().size() == 0 && "".split().size() == 0) { return 1; } return 0;)"), 1);
    EXPECT_EQ(run_int(R"(if ("a,,b".split(",").size() == 3) { return 1; } return 0;)"), 1); // 1 参保留空段
}

// ============================================================
// 解构（var 声明 / for-in 目标 / 解构赋值）
// ============================================================

// var 解构：按位置绑名（复用下标语义）；单元素形态；初始化器只求值一次（副作用单测）。
TEST(Compiler, VarDestructureBindsByPosition) {
    EXPECT_EQ(run_int("var [a, b] = [1, 2]; return a * 10 + b;"), 12);
    EXPECT_EQ(run_int("var [a] = [7]; return a;"), 7);
    EXPECT_EQ(run_int("var n = 0; fun f() { n = n + 1; return [1, 2]; } var [a, b] = f(); return n * 10 + a + b;"), 13);
    // 同一条 var 的多绑定与解构共存。
    EXPECT_EQ(run_int("var [a, b] = [1, 2], c = 3; return a * 100 + b * 10 + c;"), 123);
}

// var 解构：空模式与全 `_` 模式照旧求值初始化器（副作用照跑）但不取值--零访问故无越界可报。
TEST(Compiler, VarDestructureZeroAccessEvaluatesInitializer) {
    EXPECT_EQ(run_int("var n = 0; fun f() { n = n + 1; return [1]; } var [] = f(); return n;"), 1);
    EXPECT_EQ(run_int("var [_, _] = [1]; return 9;"), 9);
}

// 解构可嵌套：内层 listPattern 逐层递归绑定。
TEST(Compiler, DestructureNestsRecursively) {
    EXPECT_EQ(run_int("var [[a, b], c] = [[1, 2], 3]; return a * 100 + b * 10 + c;"), 123);
    EXPECT_EQ(run_int("var [p, [q, r]] = [1, [2, 3]]; return p * 100 + q * 10 + r;"), 123);
}

// `_` 占位：该位置不产生下标访问（故越界、缺键都不报），其余位置照常绑。
TEST(Compiler, WildcardSkipsAccess) {
    EXPECT_EQ(run_int("var [a, _, c] = [1, 2, 3]; return a * 10 + c;"), 13);
    EXPECT_EQ(run_int("var [a, _] = [5]; return a;"), 5);                          // `_` 位越界不访问
    EXPECT_EQ(run_int("var m = {1: \"x\"}; var [_, c] = m; return c.size();"), 1); // `_` 位缺键不报（c 取 m[1]）
}

// for-in 目标解构：map 迭代产出 [k, v] 对，逐位置绑循环变量（每轮 fresh 作用域）。
TEST(Compiler, ForInDestructuresPair) {
    EXPECT_EQ(run_int("var m = {\"a\": 1, \"b\": 2}; var s = 0; for ([k, v] in m) { s = s + v; } return s;"), 3);
    EXPECT_EQ(run_int("var m = {\"a\": 7}; var n = 0; for ([_, v] in m) { n = v; } return n;"), 7);
    EXPECT_EQ(run_int("var xs = [[1, 2], [3, 4]]; var s = 0; for ([a, b] in xs) { s = s + a * b; } return s;"), 14);
}

// for-in 根位 `_`：丢弃每轮值（不绑名），循环体照跑。
TEST(Compiler, ForInWildcardTargetDiscardsValue) {
    EXPECT_EQ(run_int("var n = 0; for (_ in [1, 2, 3]) { n = n + 1; } return n;"), 3);
}

// 解构赋值：写既有名（局部 / upvalue / 全局）；右值先整体求值，换名交换是自然结果。
TEST(Compiler, DestructureAssignmentWritesExistingNames) {
    EXPECT_EQ(run_int("var a = 1; var b = 2; [a, b] = [b, a]; return a * 10 + b;"), 21);
    EXPECT_EQ(run_int("fun g() { var a = 0; var b = 0; [[a], b] = [[1], 2]; return a * 10 + b; } return g();"), 12);
    EXPECT_EQ(run_int("var a = 0; fun f() { var xs = [5]; [a] = xs; } f(); return a;"), 5);
    EXPECT_EQ(run_int("var a = 0; var xs = [3]; [_, a] = [0, xs[0]]; return a;"), 3);
}

// 解构赋值表达式的值 = 右值（源值留栈，与 x = v 求值为 v 同款），可作表达式参与运算。
TEST(Compiler, DestructureAssignmentValueIsRhs) {
    EXPECT_EQ(run_int("var a = 0; var xs = [7]; var y = ([a] = xs); return y[0] * 10 + a;"), 77);
}

// 解构的失败面：位置不足复用下标越界；源不可下标取按类型错；解构赋值目标未声明按赋值不隐式创建；
// 新名同作用域重名按声明判重。
TEST(Compiler, DestructureFailureFaces) {
    auto short_source = run_source("var [a, b] = [1]; return 0;");
    ASSERT_FALSE(short_source.has_value());
    EXPECT_EQ(short_source.error().code(), ErrorCode::IndexOutOfBounds);

    auto unsubscriptable = run_source("var [a] = 5; return 0;");
    ASSERT_FALSE(unsubscriptable.has_value());
    EXPECT_EQ(unsubscriptable.error().code(), ErrorCode::TypeMismatch);

    auto undeclared_target = run_source("[zz] = [1]; return 0;");
    ASSERT_FALSE(undeclared_target.has_value());
    EXPECT_EQ(undeclared_target.error().code(), ErrorCode::UndefinedVariable);

    auto duplicate_name = run_source("var [a, a] = [1, 2]; return 0;");
    ASSERT_FALSE(duplicate_name.has_value());
    EXPECT_EQ(duplicate_name.error().code(), ErrorCode::RedefinedVariable);
}

// rest 位（...rest）：收集位置数之后的剩余为新 list；空尾给空 list（源恰比前缀多零个元素、
// 源为空皆然），故 head/tail 惯用法成立；嵌套与解构赋值同支持。
TEST(Compiler, DestructureRestCollectsSuffix) {
    EXPECT_EQ(run_int("var [h, ...t] = [1, 2, 3]; return h * 100 + t.size() * 10 + t[0];"), 122);
    EXPECT_EQ(run_int("var [h, ...t] = [1]; return h * 10 + t.size();"), 10); // 空尾
    EXPECT_EQ(run_int("var [...t] = []; return t.size();"), 0);               // 空源仅 rest
    EXPECT_EQ(run_int("var [...t] = [1, 2]; return t.size();"), 2);           // rest 覆盖全表
    // 前缀为 `_` 位时该位置不访问，但 rest 起点仍按位置数算（t 自位置 1 起）。
    EXPECT_EQ(run_int("var [_, ...t] = [1, 2, 3]; return t.size() * 10 + t[0];"), 22);
    // 嵌套 listPattern 里的 rest。
    EXPECT_EQ(run_int("var [[h, ...t], b] = [[1, 2, 3], 4]; return h * 100 + t.size() * 10 + b;"), 124);
    // 解构赋值里的 rest（右值先整体求值，rest 也是新 list）。
    EXPECT_EQ(run_int("var h = 0; var t = []; [h, ...t] = [7, 8, 9]; return h * 100 + t.size() * 10 + t[0];"), 728);
    // for-in 目标里的 rest：map 迭代产出 [k, v] 对，rest 收剩余位置。
    EXPECT_EQ(run_int("var m = {\"a\": 1}; var n = 0; for ([k, ...rest] in m) { n = rest.size(); } return n;"), 1);
}

// rest 落在 string 源上走切片语义（Range 下标已支持）：前缀位绑单字节串、rest 位绑字节后缀。
TEST(Compiler, DestructureRestOnStringSlicesSuffix) {
    EXPECT_EQ(run_int(R"(var [c, ...r] = "abc"; if (c == "a" && r == "bc") { return 1; } return 0;)"), 1);
    // 字节域：héllo 的 rest 位是余下 5 字节，不是「剩余字符」。
    EXPECT_EQ(run_int(R"(var [h, ...rest] = "héllo"; if (h == "h" && rest.size() == 5) { return 1; } return 0;)"), 1);
}
