#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "bytecode/CodeUnit.hpp"
#include "compile/CodeGen.hpp"
#include "compile/Lexer.hpp"
#include "compile/Parser.hpp"
#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "util/source_file.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::CodeGen;
using aria::Error;
using aria::ErrorCode;
using aria::GC;
using aria::i64;
using aria::Lexer;
using aria::new_module;
using aria::new_string;
using aria::ObjFunction;
using aria::ObjModule;
using aria::Parser;
using aria::Result;
using aria::SourceFile;
using aria::Value;

namespace {

    // 端到端：源码 -> tokenize -> parse -> CodeGen::compile。
    // 断言词法/语法成功（测试用例均用合法语法），返回编译结果（入口 ObjFunction 或首错 Error）。
    // 注意：返回的 ObjFunction 及其常量池 ObjString 归属调用方提供的 vm 的 GC，须在 vm 存活期间使用。
    Result<ObjFunction*, Error> compile_source(GC& gc, ObjModule& module, std::string_view src) {
        SourceFile file{"<test>", "<test>", aria::String{src}};
        Lexer      lexer;
        auto       lex = lexer.tokenize(&file);
        if (!lex.has_value()) {
            return std::unexpected(lex.error()[0]);
        }
        Parser parser;
        auto   parse = parser.parse(std::move(lex.value()));
        if (!parse.has_value()) {
            return std::unexpected(parse.error()[0]);
        }
        CodeGen codegen{gc};
        return codegen.compile(*parse.value(), module);
    }

    // run_source / compile_only 各自持有一个 AriaVM（进而持其 GC），并随结果一并返回，
    // 使返回值中引用的 GC 对象（ObjString 常量、Obj 返回值）在调用方检视期间存活--
    // 否则辅助函数返回即销毁局部 vm -> GC 回收 -> 悬垂引用（use-after-free）。
    // 转发 has_value/value/error 以便调用点直接当 Result 用。
    struct RunResult {
        std::unique_ptr<AriaVM> vm;
        Result<Value, Error>    result;
        bool                    has_value() const noexcept { return result.has_value(); }
        Value&                  value() noexcept { return result.value(); }
        const Value&            value() const noexcept { return result.value(); }
        Error&                  error() noexcept { return result.error(); }
        const Error&            error() const noexcept { return result.error(); }
    };

    struct Compiled {
        std::unique_ptr<AriaVM>     vm;
        Result<ObjFunction*, Error> result;
        bool                        has_value() const noexcept { return result.has_value(); }
        ObjFunction*                value() const noexcept { return result.value(); }
        Error&                      error() noexcept { return result.error(); }
        const Error&                error() const noexcept { return result.error(); }
    };

    // 端到端：源码 -> 编译 -> VM 运行。返回 RunResult（持 vm 活到调用方检视完返回值）。
    // stress GC：每次 new_object / 循环回边都 collect，主动锻炼 compile+run 的 GC 根接线，
    // 暴露缺失根（裸指针跨分配）的 bug。module 经 compile() 的 module_guard 根化、值栈/帧经
    // vm_roots tracer 标根，故 stress 下安全。
    RunResult run_source(std::string_view src) {
        auto  vm = std::make_unique<AriaVM>();
        auto& gc = vm->gc();
        gc.set_stress(true);
        auto mod_name = new_string(gc, "<test>");
        auto guard    = gc.make_guard(mod_name); // 工厂不再守卫入参:name 裸持跨 new_module 的 new_string(cwd)
        auto module   = new_module(gc, mod_name);
        auto compiled = compile_source(gc, *module, src);
        if (!compiled.has_value()) {
            return RunResult{std::move(vm), std::unexpected(compiled.error())};
        }
        auto result = vm->run(compiled.value()); // vm 活着时取结果
        return RunResult{std::move(vm), std::move(result)};
    }

    // 仅编译（不入 VM），供反汇编 / 编译期错误测试用。返回 Compiled（持 vm 活到反汇编/检视完）。
    // 同 run_source 开 stress GC，锻炼编译期根接线。
    Compiled compile_only(std::string_view src) {
        auto  vm = std::make_unique<AriaVM>();
        auto& gc = vm->gc();
        gc.set_stress(true);
        auto mod_name = new_string(gc, "<test>");
        auto guard    = gc.make_guard(mod_name); // 工厂不再守卫入参:name 裸持跨 new_module 的 new_string(cwd)
        auto module   = new_module(gc, mod_name);
        auto compiled = compile_source(gc, *module, src);
        return Compiled{std::move(vm), std::move(compiled)};
    }

    // 便捷：断言运行成功并返回整数（整数即值，无 GC 对象依赖，但仍经 RunResult 在 vm 存活期取值）。
    i64 run_int(std::string_view src) {
        auto out = run_source(src);
        EXPECT_TRUE(out.has_value()) << "expected success";
        return out.has_value() ? out.value().as_int() : 0;
    }

} // namespace

// ============================================================
// 算术 / 字面量
// ============================================================

TEST(CodeGen, ArithmeticPrecedence) {
    EXPECT_EQ(run_int("return 1 + 2 * 3;"), 7);
    EXPECT_EQ(run_int("return (1 + 2) * 3;"), 9);
    EXPECT_EQ(run_int("return 10 - 2 - 3;"), 5); // 左结合
}

TEST(CodeGen, IntDivAndMod) {
    EXPECT_EQ(run_int("return 17 / 5;"), 3);
    EXPECT_EQ(run_int("return 17 % 5;"), 2);
}

TEST(CodeGen, FloatConstantAndPromotion) {
    auto out = run_source("return 2.5 + 0.5;");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_f64());
    EXPECT_DOUBLE_EQ(out.value().as_f64(), 3.0);
}

TEST(CodeGen, IntFloatPromotion) {
    auto out = run_source("return 1 + 2.0;");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out.value().is_f64());
    EXPECT_DOUBLE_EQ(out.value().as_f64(), 3.0);
}

TEST(CodeGen, ComparisonAndEquality) {
    auto gt = run_source("return 3 > 2;");
    ASSERT_TRUE(gt.has_value());
    ASSERT_TRUE(gt.value().is_bool());
    EXPECT_TRUE(gt.value().as_bool());

    auto lt = run_source("return 1 < 0;");
    ASSERT_TRUE(lt.has_value());
    ASSERT_TRUE(lt.value().is_bool());
    EXPECT_FALSE(lt.value().as_bool());

    EXPECT_TRUE(run_source("return 1 == 1;").value().as_bool());
    EXPECT_FALSE(run_source("return 1 != 1;").value().as_bool());
    // 严格相等：1 === 1.0 为假（类型不同）
    EXPECT_FALSE(run_source("return 1 === 1.0;").value().as_bool());
    EXPECT_TRUE(run_source("return 1 !== 1.0;").value().as_bool());
}

TEST(CodeGen, Literals) {
    auto t = run_source("return true;");
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(t.value().is_bool());
    EXPECT_TRUE(t.value().as_bool());

    auto f = run_source("return false;");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(f.value().as_bool());

    auto n = run_source("return nil;");
    ASSERT_TRUE(n.has_value());
    EXPECT_TRUE(n.value().is_nil());

    auto s = run_source("return \"hello\";");
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s.value().is_obj());
    EXPECT_EQ(aria::format_value(s.value()), "hello");
}

TEST(CodeGen, UnaryMinusAndNot) {
    EXPECT_EQ(run_int("return -5;"), -5);
    EXPECT_EQ(run_int("return -(-3);"), 3);
    auto nt = run_source("return !false;");
    ASSERT_TRUE(nt.has_value());
    ASSERT_TRUE(nt.value().is_bool());
    EXPECT_TRUE(nt.value().as_bool());
    // Lua 真值：!nil -> true
    EXPECT_TRUE(run_source("return !nil;").value().as_bool());
}

// ============================================================
// 逻辑短路
// ============================================================

TEST(CodeGen, LogicShortCircuit) {
    // nil || 5 -> 5（短路真，留 lhs 即 nil? 否：|| 真跳留值，nil 假故求 rhs -> 5）
    auto or1 = run_source("return nil || 5;");
    ASSERT_TRUE(or1.has_value());
    ASSERT_TRUE(or1.value().is_int());
    EXPECT_EQ(or1.value().as_int(), 5);

    // true || x -> true（短路，不求 x）
    auto or2 = run_source("return true || nil;");
    ASSERT_TRUE(or2.has_value());
    ASSERT_TRUE(or2.value().is_bool());
    EXPECT_TRUE(or2.value().as_bool());

    // nil && 5 -> nil（短路假，留 lhs）
    auto and1 = run_source("return nil && 5;");
    ASSERT_TRUE(and1.has_value());
    EXPECT_TRUE(and1.value().is_nil());

    // 3 && 5 -> 5（lhs 真，求 rhs）
    auto and2 = run_source("return 3 && 5;");
    ASSERT_TRUE(and2.has_value());
    ASSERT_TRUE(and2.value().is_int());
    EXPECT_EQ(and2.value().as_int(), 5);
}

// ============================================================
// 变量 / 作用域 / 赋值
// ============================================================

TEST(CodeGen, GlobalVar) {
    EXPECT_EQ(run_int("var x = 5; return x;"), 5);
    EXPECT_EQ(run_int("var x = 5; x = 10; return x;"), 10);
}

TEST(CodeGen, BlockLocal) {
    // s 顶层全局；y 块内局部
    EXPECT_EQ(run_int("var s = 0; { var y = 3; s = s + y; } return s;"), 3);
}

TEST(CodeGen, Shadowing) {
    // 内层 x 遮蔽外层（外层为全局，内层为局部）
    EXPECT_EQ(run_int("var x = 1; { var x = 2; } return x;"), 1);
}

TEST(CodeGen, CompoundAssignment) {
    EXPECT_EQ(run_int("var a = 1; a += 2; return a;"), 3);
    EXPECT_EQ(run_int("var a = 10; a -= 3; return a;"), 7);
    EXPECT_EQ(run_int("var a = 4; a *= 3; return a;"), 12);
    EXPECT_EQ(run_int("var a = 20; a /= 4; return a;"), 5);
    EXPECT_EQ(run_int("var a = 17; a %= 5; return a;"), 2);
}

TEST(CodeGen, PreIncDec) {
    EXPECT_EQ(run_int("var a = 5; ++a; return a;"), 6);
    EXPECT_EQ(run_int("var a = 5; --a; return a;"), 4);
    // 前置 ++ 作为表达式留新值
    auto e = run_source("var a = 5; return ++a;");
    ASSERT_TRUE(e.has_value());
    ASSERT_TRUE(e.value().is_int());
    EXPECT_EQ(e.value().as_int(), 6);
}

// ============================================================
// 控制流
// ============================================================

TEST(CodeGen, IfElse) {
    EXPECT_EQ(run_int("var x = 3; if (x > 2) { return 1; } else { return 2; }"), 1);
    EXPECT_EQ(run_int("var x = 1; if (x > 2) { return 1; } else { return 2; }"), 2);
    EXPECT_EQ(run_int("var x = 1; if (x > 2) { return 1; } return 3;"), 3);
}

TEST(CodeGen, WhileLoop) {
    EXPECT_EQ(run_int("var i = 0; var s = 0; while (i < 5) { s = s + i; i = i + 1; } return s;"), 10);
}

TEST(CodeGen, ForLoop) {
    EXPECT_EQ(run_int("var s = 0; for (var i = 0; i < 5; i = i + 1) { s = s + i; } return s;"), 10);
}

TEST(CodeGen, BreakAndContinue) {
    // i=0,1,2(continue),3,4 -> s = 0+1+3+4 = 8；i=5 break
    EXPECT_EQ(run_int("var s = 0; for (var i = 0; i < 10; i = i + 1) {"
                      "  if (i == 5) { break; }"
                      "  if (i == 2) { continue; }"
                      "  s = s + i;"
                      "} return s;"),
              8);
}

TEST(CodeGen, WhileBreak) {
    EXPECT_EQ(run_int("var i = 0; var s = 0; while (true) {"
                      "  if (i >= 3) { break; }"
                      "  s = s + i; i = i + 1;"
                      "} return s;"),
              3);
}

// break/continue 只能 emit POP_N(运行期弹栈),不得破坏编译期 locals_ 登记:
// 跳转后的死代码仍在作用域内,引用循环体局部应解析为局部而非误落全局(否则运行期 UndefinedVariable)。
// 旧实现 emit_pop_locals_to 走 pop_locals_deeper_than 会 pop_back 移除 x -> 后续 s = s + x 解析到全局 x 报错。
TEST(CodeGen, BreakPreservesLocalsForDeadCode) {
    EXPECT_EQ(run_int("var i = 0; var s = 0; while (i < 5) {"
                      "  var x = i;"
                      "  if (i == 2) { break; }"
                      "  s = s + x; i = i + 1;"
                      "} return s;"),
              1);
}

TEST(CodeGen, ContinuePreservesLocalsForDeadCode) {
    EXPECT_EQ(run_int("var s = 0; for (var i = 0; i < 5; i = i + 1) {"
                      "  var x = i;"
                      "  if (i == 2) { continue; }"
                      "  s = s + x;"
                      "} return s;"),
              8);
}

TEST(CodeGen, IfExpr) {
    EXPECT_EQ(run_int("var c = true; return if (c) { 1 } else { 2 };"), 1);
    EXPECT_EQ(run_int("var c = false; return if (c) { 1 } else { 2 };"), 2);
}

// ============================================================
// 函数 / 调用 / 递归 / lambda
// ============================================================

TEST(CodeGen, FunctionCall) { EXPECT_EQ(run_int("fun add(a, b) { return a + b; } return add(3, 4);"), 7); }

TEST(CodeGen, Recursion) {
    EXPECT_EQ(run_int("fun fib(n) {"
                      "  if (n < 2) { return n; }"
                      "  return fib(n - 1) + fib(n - 2);"
                      "} return fib(10);"),
              55);
}

TEST(CodeGen, LocalInFunction) { EXPECT_EQ(run_int("fun f() { var x = 10; return x + 5; } return f();"), 15); }

TEST(CodeGen, Lambda) { EXPECT_EQ(run_int("var f = fun(x) { return x * 2; }; return f(21);"), 42); }

TEST(CodeGen, HigherOrderReturn) {
    // 函数作为返回值（无 upvalue 捕获）
    EXPECT_EQ(run_int("fun make() { return fun(x) { return x + 1; }; }"
                      "var g = make(); return g(41);"),
              42);
}

// ============================================================
// M4 闭包（compile 翻转后端到端：捕获读/写/共享/关闭/递归自捕获/unwind 幸存）
// ============================================================

// 路线表验收样例（vm-design §6 M4 标准）：计数器闭包 -- 内层 lambda 捕获外层局部 n,
// 捕获即引用（n = n + 1 经 STORE_UPVALUE 写穿）,外层帧 RETURN 后 upvalue 已关闭,
// 闭包存活且状态跨调用延续。
TEST(CodeGen, CounterClosure) {
    EXPECT_EQ(run_int("fun make_counter() {"
                      "  var n = 0;"
                      "  return fun() { n = n + 1; return n; };"
                      "}"
                      "var c = make_counter();"
                      "c();"
                      "c();"
                      "return c();"),
              3);
}

// 双闭包共享同一外层局部:同一 (is_local,index) 去重复用 -> 同一 ObjUpvalue 引用,
// 经 inc 的写入对 get 可见。
TEST(CodeGen, TwoClosuresShareUpvalue) {
    EXPECT_EQ(run_int("fun make() {"
                      "  var n = 0;"
                      "  var inc = fun() { n = n + 1; return n; };"
                      "  var get = fun() { return n; };"
                      "  inc();"
                      "  inc();"
                      "  return get();"
                      "}"
                      "return make();"),
              2);
}

// 引用语义：捕获是栈槽引用而非值拷贝,捕获后外层的修改对内层可见（Lua 语义）。
TEST(CodeGen, CaptureIsReference) {
    EXPECT_EQ(run_int("fun make() {"
                      "  var x = 1;"
                      "  var f = fun() { return x; };"
                      "  x = 42;"
                      "  return f();"
                      "}"
                      "return make();"),
              42);
}

// 块出作用域 -> 被捕获局部经 CLOSE_UPVALUE 关闭(值迁入 upvalue 自持),闭包仍读得到。
TEST(CodeGen, ClosureReadsClosedValueAfterBlock) {
    EXPECT_EQ(run_int("fun make() {"
                      "  var f;"
                      "  {"
                      "    var x = 7;"
                      "    f = fun() { return x; };"
                      "  }"
                      "  return f();"
                      "}"
                      "return make();"),
              7);
}

// 嵌套具名 fun 递归自捕获:名字是外层局部,内层经 upvalue 回递到自己(每次递归调用
// 现场 CLOSURE 捕获外层槽,槽里是同名闭包本身)。
TEST(CodeGen, NestedNamedFunSelfCapture) {
    EXPECT_EQ(run_int("fun outer() {"
                      "  fun fact(n) {"
                      "    if (n <= 1) { return 1; }"
                      "    return n * fact(n - 1);"
                      "  }"
                      "  return fact(5);"
                      "}"
                      "return outer();"),
              120);
}

// 异常跨帧 unwind 后幸存闭包读值:thrower 帧未命中逐帧退出,make 帧命中 handler
// (截栈只弹 try 体区间,不伤 x 的槽),make RETURN 时关闭 x 的 upvalue,闭包照常读。
TEST(CodeGen, ClosureSurvivesUnwind) {
    EXPECT_EQ(run_int("fun thrower() { throw \"boom\"; }"
                      "fun make() {"
                      "  var x = 41;"
                      "  var f = fun() { return x + 1; };"
                      "  try { thrower(); } catch (e) { }"
                      "  return f();"
                      "}"
                      "return make();"),
              42);
}

// upvalue 写穿透(引用语义):++counter 经 LOAD_UPVALUE/STORE_UPVALUE 在闭包外累计。
TEST(CodeGen, UpvalueWriteThrough) {
    EXPECT_EQ(run_int("fun make() {"
                      "  var n = 0;"
                      "  var inc = fun() { ++n; };"
                      "  inc();"
                      "  inc();"
                      "  inc();"
                      "  return n;"
                      "}"
                      "return make();"),
              3);
}

// ============================================================
// 反汇编核对（前向正确特性，VM 暂不能跑）
// ============================================================

TEST(CodeGen, ForInDisassembly) {
    auto compiled = compile_only("for (x in iter) { print x; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled.value()->unit().disassemble("<test>");
    // 迭代协议：LOAD_FIELD "iter" / "has_next" / "next" + CALL
    EXPECT_NE(text.find("LOAD_FIELD"), aria::String::npos);
    EXPECT_NE(text.find("iter"), aria::String::npos);
    EXPECT_NE(text.find("has_next"), aria::String::npos);
    EXPECT_NE(text.find("next"), aria::String::npos);
    EXPECT_NE(text.find("JUMP_BACK"), aria::String::npos);
    EXPECT_NE(text.find("JUMP_FALSE"), aria::String::npos);
}

// 顶层 import：IMPORT path 压模块值 + DEF_GLOBAL alias 绑全局。
TEST(CodeGen, ImportEmitsImport) {
    auto compiled = compile_only("import \"lib/utils\" as U;");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled.value()->unit().disassemble("<test>");
    EXPECT_NE(text.find("IMPORT"), aria::String::npos);
    EXPECT_NE(text.find("lib/utils"), aria::String::npos);
    EXPECT_NE(text.find("DEF_GLOBAL"), aria::String::npos); // 顶层经 DEF_GLOBAL 绑全局
}

// 闭包 lowering：具名 fun / lambda 一律发 CLOSURE fn_idx（不再 LOAD_CONST fn），
// 捕获描述存 ObjFunction 元数据、不进字节码流。
TEST(CodeGen, ClosureDisassembly) {
    auto compiled = compile_only("fun make() { var x = 1; return fun() { return x; }; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled.value()->unit().disassemble("<test>");
    EXPECT_NE(text.find("CLOSURE"), aria::String::npos); // 函数值经 CLOSURE 上栈
    EXPECT_EQ(text.find("LOAD_CONST"), aria::String::npos)
            << "入口 unit 不应再发 LOAD_CONST fn（x=1 走 LOAD_IMM,无其他常量加载）";
}

// for-in per-iteration 出口的新鲜绑定语义:pattern 变量被捕获 -> 每轮 end_scope 发
// CLOSE_UPVALUE（位置在 CLOSURE 绑定之后、回边 JUMP_BACK 之前）,下一轮捕获是全新 upvalue。
TEST(CodeGen, ForInPerIterationCloseUpvalue) {
    auto compiled = compile_only("for (x in iter) { var f = fun() { return x; }; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text        = compiled.value()->unit().disassemble("<test>");
    const auto closure_pos = text.find("CLOSURE");
    const auto close_pos   = text.find("CLOSE_UPVALUE");
    const auto back_pos    = text.find("JUMP_BACK");
    ASSERT_NE(closure_pos, aria::String::npos);
    ASSERT_NE(close_pos, aria::String::npos) << "被捕获的 pattern 局部须在每轮出口 CLOSE_UPVALUE";
    ASSERT_NE(back_pos, aria::String::npos);
    EXPECT_LT(closure_pos, close_pos) << "先建闭包捕获,后关槽";
    EXPECT_LT(close_pos, back_pos) << "per-iteration 出口在回边之前";
}

// 无捕获的 for-in 不发 CLOSE_UPVALUE（pattern 局部未被捕获,纯 POP_N）。
TEST(CodeGen, ForInNoCloseWithoutCapture) {
    auto compiled = compile_only("for (x in iter) { print x; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled.value()->unit().disassemble("<test>");
    EXPECT_EQ(text.find("CLOSE_UPVALUE"), aria::String::npos);
}

// ============================================================
// 编译期语义错误（compile() 返回 Error）
// ============================================================

TEST(CodeGen, ErrRedefinedVariable) {
    auto c = compile_only("var x = 1; var x = 2;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::RedefinedVariable);
}

TEST(CodeGen, ErrRedefinedGlobalFun) {
    auto c = compile_only("fun f() { return 1; } var f = 2;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::RedefinedVariable);
}

// import 别名入表：import "x" as U; 后再 var U = 1; 应报 RedefinedVariable（重构新增检查）。
TEST(CodeGen, ErrRedefinedImportAlias) {
    auto c = compile_only("import \"lib/u\" as U; var U = 1;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::RedefinedVariable);
}

// 嵌套 import（函数体内）按当前作用域绑局部：IMPORT 压值在 declare 的 slot（值填槽）+
// mark_initialized，无 DEF_GLOBAL。编译成功（不再限制仅顶层）。IMPORT 在嵌套函数 f 自己的
// unit 里（不在入口 unit），故此处只验编译成功，字节码形状由 ImportNestedInBlock 在入口 unit 验。
TEST(CodeGen, ImportNestedInFunction) {
    auto c = compile_only("fun f() { import \"lib/u\" as U; }");
    ASSERT_TRUE(c.has_value()) << "嵌套 import 应编译成功（绑局部）";
}

// 块作用域内的 import 按嵌套局部绑定（IMPORT 在入口 unit）：IMPORT 压值 + 值填槽，无 DEF_GLOBAL。
TEST(CodeGen, ImportNestedInBlock) {
    auto c = compile_only("{ import \"lib/u\" as U; }");
    ASSERT_TRUE(c.has_value()) << "块内 import 应编译成功（绑局部）";
    const auto text = c.value()->unit().disassemble("<test>");
    EXPECT_NE(text.find("IMPORT"), aria::String::npos);
    EXPECT_EQ(text.find("DEF_GLOBAL"), aria::String::npos) << "嵌套 import 不走 DEF_GLOBAL";
}

// 使用「定义但未初始化」的局部 -> UninitializedVariable（Python 风格 definite-assignment）。
// 典型：初始化器中自引用（declare 已标记未初始化，init 尚未完成时读取）。
TEST(CodeGen, ErrUninitializedVariableSelfRef) {
    auto c = compile_only("fun f() { var x = x + 1; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::UninitializedVariable);
}

TEST(CodeGen, ErrUninitializedVariableBareSelf) {
    auto c = compile_only("fun f() { var x = x; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::UninitializedVariable);
}

// 复合赋值 / 前置自增在自身初始化器中读取未初始化局部（经 emit_load 读点检查）。
TEST(CodeGen, ErrUninitializedVariablePreIncInInit) {
    auto c = compile_only("fun f() { var x = ++x; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::UninitializedVariable);
}

// var x; 无初始化器 -> LOAD_NIL 填槽并标记初始化，后续读取合法（nil）。
TEST(CodeGen, VarNoInitializerReadsNil) {
    auto c = compile_only("fun f() { var x; var y = x; return y; }");
    ASSERT_TRUE(c.has_value()) << "var x; 后读取应合法（nil），不应报 UninitializedVariable";
}

TEST(CodeGen, VarNoInitializerRuntimeNil) {
    // 局部 var x; 使 x = nil（LOAD_NIL 填槽 + mark_init）；x == nil 成立 -> 返回 1。
    EXPECT_EQ(run_int("fun f() { var x; if (x == nil) { return 1; } return 0; } return f();"), 1);
}

// 新 var 模型：嵌套 var 的初始化器值恰好压在 slot（无 store/pop），运行时行为与旧模型等价。
TEST(CodeGen, VarInitValueFillsSlot) {
    EXPECT_EQ(run_int("fun f() { var x = 5; var y = x + 1; return y; } return f();"), 6);
}

// 嵌套具名 fun 绑定（新模型：declare + LOAD_CONST + mark_init，无 store/pop）。
TEST(CodeGen, NestedNamedFun) {
    EXPECT_EQ(run_int("fun outer() { fun inner() { return 42; } return inner(); } return outer();"), 42);
}

TEST(CodeGen, ErrBreakOutsideLoop) {
    auto c = compile_only("break;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::BreakOutsideLoop);
}

// 循环上下文随函数隔离：嵌套函数内的 break 不应绑定到外层循环（重构前 loop_stack_
// 是 CodeGen 单栈，会错误绑定；现 loop_stack_ 收入 FunctionCtx，函数边界天然隔离）。
TEST(CodeGen, ErrBreakInNestedFunDoesNotBindOuterLoop) {
    auto c = compile_only("for (var i = 0; i < 3; i = i + 1) { fun g() { break; } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::BreakOutsideLoop);
}

TEST(CodeGen, ErrContinueOutsideLoop) {
    auto c = compile_only("continue;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::ContinueOutsideLoop);
}

// 形参 > 255(arity u8 上限) -> TooManyParameters（区别于体局部超限 TooManyLocals）。
TEST(CodeGen, ErrTooManyParameters) {
    std::string src = "fun f(";
    for (int i = 0; i < 256; ++i) {
        src += "p" + std::to_string(i);
        if (i + 1 < 256) {
            src += ", ";
        }
    }
    src += ") { return 0; }";
    auto c = compile_only(src);
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::TooManyParameters);
}

// 单函数捕获超容量（u8 upvalue 索引域,容量 256）-> TooManyUpvalues。
// 源码程序生成:257 个外层局部各被内层 lambda 捕获一次,第 257 条捕获越界。
TEST(CodeGen, ErrTooManyUpvalues) {
    std::string src = "fun outer() {";
    for (int i = 0; i < 257; ++i) {
        src += "var v" + std::to_string(i) + " = " + std::to_string(i) + ";";
    }
    src += "return fun() { return";
    for (int i = 0; i < 257; ++i) {
        src += " v" + std::to_string(i) + " +";
    }
    src += " 0; }; }";
    auto c = compile_only(src);
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::TooManyUpvalues);
}

// 容量下界钉子:恰 256 个不同捕获(索引 0..255 用满 u8 索引域,kMaxUpvalues=255 为上限位置、
// 容量 = 上限 + 1,对齐 kMaxConstants 允许 65536 项与 clox UINT8_COUNT)合法编译且运行正确--
// 勿把边界「修正」为 255(那会白禁合法索引 255)。
TEST(CodeGen, ExactlyMaxUpvaluesCompiles) {
    std::string src = "fun outer() {";
    for (int i = 0; i < 256; ++i) {
        src += "var v" + std::to_string(i) + " = " + std::to_string(i) + ";";
    }
    src += "return fun() { return";
    for (int i = 0; i < 256; ++i) {
        src += " v" + std::to_string(i) + " +";
    }
    src += " 0; }; } return outer()();";
    // 0+1+...+255 = 32640:端到端跑通 CLOSURE 的 256 条捕获循环 + LOAD_UPVALUE 全索引域
    // (run_source 开 stress GC,顺带压 CLOSURE 捕获循环的根安全)。
    EXPECT_EQ(run_int(src), 32640);
}

// 捕获去重:同一外层局部被内层多处引用只占一个 upvalue -- 257 处引用同一变量不越界。
TEST(CodeGen, DedupCaptureCountsOnce) {
    std::string src = "fun outer() { var v = 5; return fun() { return";
    for (int i = 0; i < 257; ++i) {
        src += " v +";
    }
    src += " 0; }; }";
    auto c = compile_only(src);
    ASSERT_TRUE(c.has_value()) << "同一 (is_local,index) 去重复用,257 处引用只登记 1 条";
}

TEST(CodeGen, ErrInvalidAssignmentTarget) {
    auto c = compile_only("1 = 2;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::InvalidAssignmentTarget);
}

TEST(CodeGen, ErrTryWithoutHandler) {
    auto c = compile_only("try { print 1; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::TryWithoutHandler);
}

TEST(CodeGen, ErrDuplicateParam) {
    auto c = compile_only("fun f(a, a) { return a; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::DuplicateParam);
}

TEST(CodeGen, ErrNumberOutOfRange) {
    // i48 上界 2^47-1 = 140737488355327；此值在 i64 内（lex 通过）但超 i48 -> NumberOutOfRange。
    auto c = compile_only("return 999999999999999;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NumberOutOfRange);
}

TEST(CodeGen, ErrNotImplementedDef) {
    auto c = compile_only("def X { }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NotImplemented);
}

TEST(CodeGen, ErrNotImplementedListLiteral) {
    auto c = compile_only("return [1, 2, 3];");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NotImplemented);
}

TEST(CodeGen, ErrNotImplementedFieldAccess) {
    auto c = compile_only("var a = 1; return a.x;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NotImplemented);
}

TEST(CodeGen, ErrNotImplementedMatch) {
    auto c = compile_only("match (1) { 1 => { print 1; } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NotImplemented);
}

TEST(CodeGen, ErrNotImplementedDefaultParam) {
    auto c = compile_only("fun f(a, b = 1) { return b; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::NotImplemented);
}

// 出错即停 + ~ModuleCtx 沿 enclosing_ 链释放：lambda（表达式位）体内 break 触发 BreakOutsideLoop
// （lambda 自身 loop_stack 空，不绑外层 for），错误发生在 compile_function 摆动游标到 lambda 之后。
// 游标停在 lambda 上下文，各层 visit（for / block / var / binary / call）经 if(!ok()) return 短路、
// 不继续 emit；~ModuleCtx 沿 enclosing_ 链（lambda -> entry）逐个释放。验证不崩溃且返 BreakOutsideLoop。
TEST(CodeGen, ErrStopOnNestedLambdaInExpr) {
    auto c = compile_only("for (var i = 0; i < 3; i = i + 1) { var x = 1 + (fun() { break; })(); }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::BreakOutsideLoop);
}

// ============================================================
// 运行期错误（VM 报）
// ============================================================

TEST(CodeGen, ErrRuntimeUndefinedVariable) {
    // 裸名非局部 -> LOAD_GLOBAL；VM 运行期查表未定义 -> UndefinedVariable
    auto out = run_source("return x;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

TEST(CodeGen, ErrRuntimeAssignUndefined) {
    // 赋值不隐式创建：x 未声明 -> STORE_GLOBAL 运行期未定义 -> UndefinedVariable
    auto out = run_source("x = 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// ============================================================
// 内置函数（M2 收尾：type / len / str / assert）
// ============================================================
// 内置由 VM 级只读 builtins_ 表承载（ctor 一次注册），LOAD_GLOBAL 模块 globals 未命中后回退查之
// （Python 式 globals -> builtins 查找链，无 LOAD_BUILTIN 指令）。run_source 走 run(ObjFunction*)，
// VM ctor 已注册 builtins，故单次 run 测试天然覆盖。跨行 shadow 持久见 BuiltinShadowPersistsAcrossRuns。

TEST(CodeGen, BuiltinType) {
    // type(x) -> 值的精确类型名（PascalCase）。Int/Bool/Nil/String 各一。
    EXPECT_EQ(aria::format_value(run_source("return type(42);").value()), "Int");
    EXPECT_EQ(aria::format_value(run_source("return type(true);").value()), "Bool");
    EXPECT_EQ(aria::format_value(run_source("return type(nil);").value()), "Nil");
    EXPECT_EQ(aria::format_value(run_source("return type(\"x\");").value()), "String");
}

TEST(CodeGen, BuiltinLen) {
    // len(s) -> 字符串长度（当前仅 String，List/Map 随 M5）。
    EXPECT_EQ(run_int("return len(\"abc\");"), 3);
    EXPECT_EQ(run_int("return len(\"\");"), 0);
}

TEST(CodeGen, BuiltinLenNonString) {
    // len 对非字符串报 TypeMismatch。
    auto out = run_source("return len(42);");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

TEST(CodeGen, BuiltinStr) {
    // str(x) -> 可读渲染（同 PRINT / format_value）。
    EXPECT_EQ(aria::format_value(run_source("return str(nil);").value()), "nil");
    EXPECT_EQ(aria::format_value(run_source("return str(42);").value()), "42");
    EXPECT_EQ(aria::format_value(run_source("return str(true);").value()), "true");
}

TEST(CodeGen, BuiltinAssertPass) {
    // assert(true) 成功返 nil（作为语句被 POP 丢弃），后续正常执行。
    EXPECT_EQ(run_int("assert(true); return 1;"), 1);
    // 真值（非 false/nil）亦通过：Lua 风格真值。
    EXPECT_EQ(run_int("assert(1); return 2;"), 2);
}

TEST(CodeGen, BuiltinAssertFail) {
    // assert(false) -> AssertionFailed，未捕获即 run() 返回该错误。
    auto out = run_source("assert(false); return 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::AssertionFailed);
}

TEST(CodeGen, BuiltinAssertFailWithMessage) {
    // assert(false, "boom") -> AssertionFailed，消息含自定义串。
    auto out = run_source("assert(false, \"boom\"); return 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::AssertionFailed);
    EXPECT_TRUE(out.error().message().find("boom") != std::string::npos);
}

TEST(CodeGen, BuiltinShadowedByUserGlobal) {
    // 用户顶层 var 同名覆盖内置：DEF_GLOBAL 在运行期 upsert 覆写同名全局，内置被替换。
    // 内置仅注册进运行期 globals 表，不入编译期 defined_globals_，故 var len 不触发 RedefinedVariable。
    EXPECT_EQ(run_int("var len = 5; return len;"), 5);
    EXPECT_EQ(run_int("var type = 99; return type;"), 99);
}

TEST(CodeGen, BuiltinArityCheck) {
    // 内置自检 argc：type() 0 参 / type(1,2) 2 参 -> WrongArity。
    auto a = run_source("return type();");
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code(), ErrorCode::WrongArity);

    auto b = run_source("return type(1, 2);");
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().code(), ErrorCode::WrongArity);
}

TEST(CodeGen, BuiltinAssertArityCheck) {
    // assert 自检 argc：assert() 0 参 / assert(1,2,3) 3 参 -> WrongArity（修零参越界读 slots[1]）。
    auto a = run_source("assert(); return 1;");
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code(), ErrorCode::WrongArity);

    auto b = run_source("assert(1, 2, 3); return 1;");
    ASSERT_FALSE(b.has_value());
    EXPECT_EQ(b.error().code(), ErrorCode::WrongArity);
}

TEST(CodeGen, BuiltinShadowPersistsAcrossRuns) {
    // 跨 run() 复用同一模块（模拟 REPL 逐行）：第 1 行 `var len = 5` 写入模块 globals；
    // 第 2 行 `return len` 应命中模块 globals 返回 5，而非被内置覆写回 <fn len>。
    // 旧方案 A「每模块预填 globals」会在第 2 行 run() 重注册、冲掉 shadow；方案 B VM 级只读
    // builtins_ 表不再每 run 注入，shadow 跨行持久。stress GC 锻炼 builtins_ 根接线。
    auto  vm = std::make_unique<AriaVM>();
    auto& gc = vm->gc();
    gc.set_stress(true);
    auto mod_name = new_string(gc, "<test>");
    auto guard    = gc.make_guard(mod_name);
    auto module   = new_module(gc, mod_name);

    auto c1 = compile_source(gc, *module, "var len = 5;");
    ASSERT_TRUE(c1.has_value());
    auto r1 = vm->run(c1.value());
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    auto c2 = compile_source(gc, *module, "return len;");
    ASSERT_TRUE(c2.has_value());
    auto r2 = vm->run(c2.value());
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
    EXPECT_EQ(r2.value().as_int(), 5); // 非内置 <fn len>
}

TEST(CodeGen, BuiltinBareAssignWithoutVarFails) {
    // 裸名赋值 `len = 5`（无 var 声明）：模块 globals 未命中 -> UndefinedVariable，不回退 builtins
    // 写（STORE_GLOBAL 不回退，与 grammar §205-206「赋值不隐式创建、必须先 var 声明」一致）。
    // 旧方案 A 因预填会静默覆写内置；方案 B 正确报错。
    auto out = run_source("len = 5; return len;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// ============================================================
// M3 异常（try/catch/throw）
// ============================================================
// 统一异常通道：aria throw 与 VM 运行时错误都走挂起寄存器 + unwind 查 CodeUnit 异常记录表
// （无 SETUP_EXCEPT 指令，不依赖 C++ 异常）。单寄存器模型：用户 throw 存原值保类型、运行时
// 错误装箱 ObjException 携码；未捕获物化 Error（ObjException 经 from_baked 保码 / 原值兜底
// UncaughtException）并烘焙逐帧堆栈跟踪（外 -> 内）。run_source 的 stress GC 默认开，锻炼
// pending_error_ 根接线（pitfalls 坑 #8）。合成模块 <test> 的位置前缀退化 "<test>:line"。

TEST(CodeGen, ThrowIntCaughtBindsValue) {
    // throw 42 被 catch 捕获，e 绑原值保类型（单寄存器模型，坑 #7）。
    EXPECT_EQ(run_int("try { throw 42; } catch (e) { return e; }"), 42);
}

TEST(CodeGen, ThrowStringCaughtBindsValue) {
    auto out = run_source("try { throw \"boom\"; } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    // 渲染即串内容，证绑的是 ObjString 原值（非消息串）。
    EXPECT_EQ(aria::format_value(out.value()), "boom");
}

TEST(CodeGen, UncaughtUserThrowIsUncaughtException) {
    // throw 42 未捕获：原值兜底物化 UncaughtException（无位置前缀 -- throw 不装箱，位置由
    // 跟踪的 at 行给出），消息渲染值本身 + at 跟踪行（throw 站点 = 顶帧）。
    auto out = run_source("throw 42;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UncaughtException);
    EXPECT_EQ(out.error().message(), "Runtime: UncaughtException uncaught exception: 42\n"
                                     "  at <main> (<test>:1)");
}

TEST(CodeGen, RuntimeErrorCaughtBindsObjException) {
    // 运行时错误（除零）可捕获：e 绑 ObjException（携码 + 完整烘焙消息，print/str 渲染之）。
    auto out = run_source("try { return 1 / 0; } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    ASSERT_TRUE(out.value().is_obj());
    const auto* ex = aria::Object::as<aria::ObjException>(out.value().as_obj());
    ASSERT_NE(ex, nullptr);
    EXPECT_EQ(ex->code(), ErrorCode::DivisionByZero);
    EXPECT_EQ(ex->to_string(), "<test>:1: Runtime: DivisionByZero integer division by zero");
}

TEST(CodeGen, RethrowPreservesCode) {
    // re-throw 保码（单寄存器收益，坑 #7）：catch 绑 ObjException 再 throw（catch 体不在
    // 本层受保护区间内，向外传播），未捕获经 from_baked 回 DivisionByZero（非 UncaughtException）。
    auto out = run_source("try { return 1 / 0; } catch (e) { throw e; }");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::DivisionByZero);
    EXPECT_EQ(out.error().message(), "<test>:1: Runtime: DivisionByZero integer division by zero\n"
                                     "  at <main> (<test>:1)");
}

TEST(CodeGen, NativeFailCaughtByTry) {
    // 原生报错（len 非 String）同走异常通道：vm.fail 装箱 ObjException 入寄存器，CALL 失败
    // 经 unwind 被捕获；str(e) 渲染完整消息，位置 = CALL 站点行（原生不进帧，坑 #15）。
    auto out = run_source("try { return len(nil); } catch (e) { return str(e); }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(aria::format_value(out.value()), "<test>:1: Runtime: TypeMismatch len requires a string, got Nil");
}

TEST(CodeGen, NestedTryInnerCatches) {
    // 嵌套 try：内层捕获（find_try_handler 取最内层覆盖区间）。
    auto out = run_source("try { try { throw 1; } catch (i) { return i; } } catch (o) { return 2; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 1);
}

TEST(CodeGen, NestedTryOuterCatchesInnerRethrow) {
    // 内层 catch re-throw：THROW 指令在 catch 体（本层区间之外、外层区间之内）-> 外层捕获。
    auto out = run_source("try { try { throw 1; } catch (i) { throw i; } } catch (o) { return o + 10; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 11);
}

TEST(CodeGen, CrossFrameCatch) {
    // 跨帧捕获：被调函数 throw，unwind 逐帧 exit_frame 后在调用者帧命中 handler（坑 #13），
    // 截值栈到 frame.slots + stack_depth，异常值 push 落 catch 参数槽。
    auto out = run_source("fun f() { throw \"cross\"; } try { f(); } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(aria::format_value(out.value()), "cross");
}

TEST(CodeGen, DeepCallChainUnwind) {
    // 三层调用链（a -> b -> c）内 throw，顶层 try 捕获：unwind 连弹三层帧后派发 handler。
    auto out = run_source(R"(
fun c() {
    throw 7;
}
fun b() {
    return c();
}
fun a() {
    return b();
}
try {
    return a();
} catch (e) {
    return e;
}
)");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 7);
}

TEST(CodeGen, TryBodyLocalsDiscardedOnUnwind) {
    // unwind 截栈丢弃 try 体临时值与被调帧残留（截到 stack_depth），try 外变量与 catch 体
    // 局部照常可用 -- 栈不腐坏（坑 #6/#10 的值填槽不变式）。
    auto out = run_source(R"(
var keep = 1;
fun f() {
    var in_f = 2;
    throw 3;
}
try {
    var in_try = f();
} catch (e) {
    var in_catch = 4;
    return keep + in_catch + e;
}
)");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out.value().as_int(), 8);
}

TEST(CodeGen, UncaughtStackTraceListsFramesOuterToInner) {
    // 未捕获跨帧错误：消息尾部逐帧 at 行，外 -> 内（Python 式 most recent call last，坑 #16）；
    // 行号 = 各帧执行位置（div 的除法行 / mid 的 CALL 行 / <main> 的 CALL 行，同一 last_ip）。
    auto out = run_source(R"(
fun div() {
    return 1 / 0;
}
fun mid() {
    return div();
}
mid();
)");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::DivisionByZero);
    EXPECT_EQ(out.error().message(), "<test>:3: Runtime: DivisionByZero integer division by zero\n"
                                     "  at <main> (<test>:8)\n"
                                     "  at mid (<test>:6)\n"
                                     "  at div (<test>:3)");
}

TEST(CodeGen, FinallyIsPlainIdentifierAfterRemoval) {
    // finally 已裁撤（2026-09，善后后继 defer 已降为可选后续、不绑定 M4）：不再是关键字，回归普通标识符可绑定。
    auto c = compile_only("var finally = 1; print finally;");
    ASSERT_TRUE(c.has_value()) << c.error().message();
}

TEST(CodeGen, TryCatchEmitsTryRecordAndThrow) {
    // 发射核对：try_records 一条，受保护区间 [begin, end) 覆盖 try 体（THROW 在内），
    // handle 指向跳过 catch 的 JUMP 之后，stack_depth = try 入口局部数（<main> 顶层仅
    // slot 0 哑元 = 1）；反汇编出现 try records 小节（非空才列）。
    auto c = compile_only("try { throw 1; } catch (e) { print e; }");
    ASSERT_TRUE(c.has_value()) << c.error().message();
    const auto& cu = c.value()->unit();
    ASSERT_EQ(cu.try_records.size(), 1u);
    const auto& rec = cu.try_records[0];
    EXPECT_LT(rec.begin, rec.end);
    EXPECT_GT(rec.handle, rec.end);
    EXPECT_EQ(rec.stack_depth, 1u);
    EXPECT_NE(cu.disassemble("<main>").find("try records:"), std::string::npos);
}

TEST(CodeGen, NestedTryRecordsAscendingByBegin) {
    // 嵌套 try：入口预插占位 + 结尾回填（坑 #4）保证记录按 begin 非降序（二分查表前提），
    // 且内层区间整个嵌于外层区间内。begin 相等（内层是外层体首条语句、其间零发射）合法，
    // 查表靠反向扫描取最内层 -- 该 tie 语义由 NestedTryInnerCatches 运行期覆盖；本例内层
    // try 前有语句，begin 严格递增。
    auto c = compile_only("try { print 0; try { print 1; } catch (a) { print 2; } } catch (b) { print 3; }");
    ASSERT_TRUE(c.has_value()) << c.error().message();
    const auto& recs = c.value()->unit().try_records;
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_LT(recs[0].begin, recs[1].begin);
    EXPECT_LT(recs[1].begin, recs[0].end); // 内层起点在外层区间内
    EXPECT_LT(recs[1].end, recs[0].end);
}
