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
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
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
// 旧实现 pop_locals_to 走 pop_locals_deeper_than 会 pop_back 移除 x -> 后续 s = s + x 解析到全局 x 报错。
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

TEST(CodeGen, ImportEmitsImport) {
    auto compiled = compile_only("import \"lib/utils\" as U;");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled.value()->unit().disassemble("<test>");
    EXPECT_NE(text.find("IMPORT"), aria::String::npos);
    EXPECT_NE(text.find("lib/utils"), aria::String::npos);
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
