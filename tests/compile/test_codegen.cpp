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
    Result<ObjFunction*, Error> compile_source(GC& gc, ObjModule* module, std::string_view src) {
        SourceFile file{"<test>", "<test>", aria::String{src}};
        auto       lex = Lexer::tokenize(file);
        if (!lex) {
            return std::unexpected(std::move(lex.error()[0]));
        }
        auto parse = Parser::parse(std::move(*lex));
        if (!parse) {
            return std::unexpected(std::move(parse.error()[0]));
        }
        auto program = std::move(*parse);
        return CodeGen::compile(gc, *program, module, aria::kMainEntryName);
    }

    // run_source / compile_only 各自持有一个 AriaVM（进而持其 GC），并随结果一并返回，
    // 使返回值中引用的 GC 对象（ObjString 常量、Obj 返回值）在调用方检视期间存活--
    // 否则辅助函数返回即销毁局部 vm -> GC 回收 -> 悬垂引用（use-after-free）。
    // 转发 has_value/error 并提供 bool/->/* ，调用点按 Result 惯用法访问（CPP_Naming_Convention
    // 「Optional/Result 用法」：取值 */->，判断隐式 bool，不设 .value()）。
    struct RunResult {
        std::unique_ptr<AriaVM> vm;
        Result<Value, Error>    result;
        bool                    has_value() const noexcept { return result.has_value(); }
        explicit                operator bool() const noexcept { return has_value(); }
        Value*                  operator->() noexcept { return &*result; }
        Value&                  operator*() noexcept { return *result; }
        Error&                  error() noexcept { return result.error(); }
        const Error&            error() const noexcept { return result.error(); }
    };

    struct Compiled {
        std::unique_ptr<AriaVM>     vm;
        Result<ObjFunction*, Error> result;
        bool                        has_value() const noexcept { return result.has_value(); }
        ObjFunction*                operator->() const noexcept { return *result; }
        Error&                      error() noexcept { return result.error(); }
        const Error&                error() const noexcept { return result.error(); }
    };

    // 端到端：源码 -> 编译 -> VM 运行。返回 RunResult（持 vm 活到调用方检视完返回值）。
    // stress GC：每次 new_object / 循环回边都 collect，主动锻炼 compile+run 的 GC 根接线，
    // 暴露缺失根（裸指针跨分配）的 bug。module 经 compile() 的 guard 根化、值栈/帧经
    // vm_roots tracer 标根，故 stress 下安全。
    RunResult run_source(std::string_view src) {
        auto  vm = std::make_unique<AriaVM>();
        auto& gc = vm->gc();
        gc.set_stress(true);
        auto module   = new_module(gc, "<test>"); // StringView 重载:名字经工厂内部 intern 并自守
        auto compiled = compile_source(gc, module, src);
        if (!compiled) {
            return RunResult{std::move(vm), std::unexpected(compiled.error())};
        }
        auto result = vm->run(*compiled); // vm 活着时取结果
        return RunResult{std::move(vm), std::move(result)};
    }

    // 仅编译（不入 VM），供反汇编 / 编译期错误测试用。返回 Compiled（持 vm 活到反汇编/检视完）。
    // 同 run_source 开 stress GC，锻炼编译期根接线。
    Compiled compile_only(std::string_view src) {
        auto  vm = std::make_unique<AriaVM>();
        auto& gc = vm->gc();
        gc.set_stress(true);
        auto module   = new_module(gc, "<test>"); // StringView 重载:名字经工厂内部 intern 并自守
        auto compiled = compile_source(gc, module, src);
        return Compiled{std::move(vm), std::move(compiled)};
    }

    // 便捷：断言运行成功并返回整数（整数即值，无 GC 对象依赖，但仍经 RunResult 在 vm 存活期取值）。
    i64 run_int(std::string_view src) {
        auto out = run_source(src);
        EXPECT_TRUE(out.has_value()) << "expected success";
        return out ? out->as_int() : 0;
    }

    // 逐行扫描 disassembly 文本，断言存在「含 first 的行，其紧邻下一行含 second」的相邻对
    // （指令相邻形态断言，反汇编形状测试用）。
    bool lines_adjacent(const aria::String& text, const std::string_view first, const std::string_view second) {
        for (aria::usize pos = text.find(first); pos != aria::String::npos; pos = text.find(first, pos + 1)) {
            const aria::usize line_end = text.find('\n', pos);
            if (line_end == aria::String::npos) {
                return false; // first 在末行，无下一行
            }
            const aria::usize next_begin = line_end + 1;
            const aria::usize next_end   = text.find('\n', next_begin);
            const aria::usize second_pos = text.find(second, next_begin);
            if (second_pos != aria::String::npos && (next_end == aria::String::npos || second_pos < next_end)) {
                return true;
            }
        }
        return false;
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
    ASSERT_TRUE(out->is_f64());
    EXPECT_DOUBLE_EQ(out->as_f64(), 3.0);
}

TEST(CodeGen, IntFloatPromotion) {
    auto out = run_source("return 1 + 2.0;");
    ASSERT_TRUE(out.has_value());
    ASSERT_TRUE(out->is_f64());
    EXPECT_DOUBLE_EQ(out->as_f64(), 3.0);
}

TEST(CodeGen, ComparisonAndEquality) {
    auto gt = run_source("return 3 > 2;");
    ASSERT_TRUE(gt.has_value());
    ASSERT_TRUE(gt->is_bool());
    EXPECT_TRUE(gt->as_bool());

    auto lt = run_source("return 1 < 0;");
    ASSERT_TRUE(lt.has_value());
    ASSERT_TRUE(lt->is_bool());
    EXPECT_FALSE(lt->as_bool());

    EXPECT_TRUE((*run_source("return 1 == 1;")).as_bool());
    EXPECT_FALSE((*run_source("return 1 != 1;")).as_bool());
    // 严格相等：1 === 1.0 为假（类型不同）
    EXPECT_FALSE((*run_source("return 1 === 1.0;")).as_bool());
    EXPECT_TRUE((*run_source("return 1 !== 1.0;")).as_bool());
}

TEST(CodeGen, Literals) {
    auto t = run_source("return true;");
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(t->is_bool());
    EXPECT_TRUE(t->as_bool());

    auto f = run_source("return false;");
    ASSERT_TRUE(f.has_value());
    EXPECT_FALSE(f->as_bool());

    auto n = run_source("return nil;");
    ASSERT_TRUE(n.has_value());
    EXPECT_TRUE(n->is_nil());

    auto s = run_source("return \"hello\";");
    ASSERT_TRUE(s.has_value());
    ASSERT_TRUE(s->is_obj());
    EXPECT_EQ(aria::format_value(*s), "hello");
}

TEST(CodeGen, UnaryMinusAndNot) {
    EXPECT_EQ(run_int("return -5;"), -5);
    EXPECT_EQ(run_int("return -(-3);"), 3);
    auto nt = run_source("return !false;");
    ASSERT_TRUE(nt.has_value());
    ASSERT_TRUE(nt->is_bool());
    EXPECT_TRUE(nt->as_bool());
    // Lua 真值：!nil -> true
    EXPECT_TRUE((*run_source("return !nil;")).as_bool());
}

// ============================================================
// 逻辑短路
// ============================================================

TEST(CodeGen, LogicShortCircuit) {
    // nil || 5 -> 5（nil 假，求 rhs 留值）
    auto or1 = run_source("return nil || 5;");
    ASSERT_TRUE(or1.has_value());
    ASSERT_TRUE(or1->is_int());
    EXPECT_EQ(or1->as_int(), 5);

    // true || x -> true（短路，不求 x）
    auto or2 = run_source("return true || nil;");
    ASSERT_TRUE(or2.has_value());
    ASSERT_TRUE(or2->is_bool());
    EXPECT_TRUE(or2->as_bool());

    // nil && 5 -> nil（短路假，留 lhs）
    auto and1 = run_source("return nil && 5;");
    ASSERT_TRUE(and1.has_value());
    EXPECT_TRUE(and1->is_nil());

    // 3 && 5 -> 5（lhs 真，求 rhs）
    auto and2 = run_source("return 3 && 5;");
    ASSERT_TRUE(and2.has_value());
    ASSERT_TRUE(and2->is_int());
    EXPECT_EQ(and2->as_int(), 5);
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
    ASSERT_TRUE(e->is_int());
    EXPECT_EQ(e->as_int(), 6);
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
// M4 闭包（端到端：捕获读/写/共享/关闭/递归自捕获/unwind 幸存）
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
    const auto text = compiled->unit().disassemble("<test>");
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
    const auto text = compiled->unit().disassemble("<test>");
    EXPECT_NE(text.find("IMPORT"), aria::String::npos);
    EXPECT_NE(text.find("lib/utils"), aria::String::npos);
    EXPECT_NE(text.find("DEF_GLOBAL"), aria::String::npos); // 顶层经 DEF_GLOBAL 绑全局
}

// 闭包 lowering：具名 fun / lambda 一律发 CLOSURE fn_idx，捕获描述存 ObjFunction 元数据、不进字节码流。
TEST(CodeGen, ClosureDisassembly) {
    auto compiled = compile_only("fun make() { var x = 1; return fun() { return x; }; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text = compiled->unit().disassemble("<test>");
    EXPECT_NE(text.find("CLOSURE"), aria::String::npos); // 函数值经 CLOSURE 上栈
    EXPECT_EQ(text.find("LOAD_CONST"), aria::String::npos)
            << "入口 unit 不应再发 LOAD_CONST fn（x=1 走 LOAD_IMM,无其他常量加载）";
}

// for-in per-iteration 出口的新鲜绑定语义:pattern 变量被捕获 -> 每轮 end_scope 发
// CLOSE_UPVALUE（位置在 CLOSURE 绑定之后、回边 JUMP_BACK 之前）,下一轮捕获是全新 upvalue。
TEST(CodeGen, ForInPerIterationCloseUpvalue) {
    auto compiled = compile_only("for (x in iter) { var f = fun() { return x; }; }");
    ASSERT_TRUE(compiled.has_value());
    const auto text        = compiled->unit().disassemble("<test>");
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
    const auto text = compiled->unit().disassemble("<test>");
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

// import 别名入表：import "x" as U; 后再 var U = 1; 应报 RedefinedVariable。
TEST(CodeGen, ErrRedefinedImportAlias) {
    auto c = compile_only("import \"lib/u\" as U; var U = 1;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::RedefinedVariable);
}

// 嵌套 import（函数体内）按当前作用域绑局部：IMPORT 压值在 declare 的 slot（值填槽），
// 无 DEF_GLOBAL，编译成功。IMPORT 在嵌套函数 f 自己的 unit 里（不在入口
// unit），故此处只验编译成功，字节码形状由 ImportNestedInBlock 在入口 unit 验。
TEST(CodeGen, ImportNestedInFunction) {
    auto c = compile_only("fun f() { import \"lib/u\" as U; }");
    ASSERT_TRUE(c.has_value()) << "嵌套 import 应编译成功（绑局部）";
}

// 块作用域内的 import 按嵌套局部绑定（IMPORT 在入口 unit）：IMPORT 压值 + 值填槽，无 DEF_GLOBAL。
TEST(CodeGen, ImportNestedInBlock) {
    auto c = compile_only("{ import \"lib/u\" as U; }");
    ASSERT_TRUE(c.has_value()) << "块内 import 应编译成功（绑局部）";
    const auto text = c->unit().disassemble("<test>");
    EXPECT_NE(text.find("IMPORT"), aria::String::npos);
    EXPECT_EQ(text.find("DEF_GLOBAL"), aria::String::npos) << "嵌套 import 不走 DEF_GLOBAL";
}

// var 自引用：声明名在初始化器求值后才登记，init 里的 x 沿 resolve 链落外层。函数内无外层
// x -> 落全局，双 miss 抛运行期 UndefinedVariable（definite-assignment 已退役，编译期放行）。
TEST(CodeGen, VarSelfRefUndefinedGlobalAtRuntime) {
    auto out = run_source("fun f() { var x = x + 1; return x; } return f();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// 捕获语义钉子：外层 x 是 make 的局部，g 的 init `x + 1` 经 upvalue 捕获读外层 x，随后登记
// g 自己的局部 x -> g 的 x = 外层 x + 1，「新 x 用外层 x」语义成立。
TEST(CodeGen, VarInitCapturesOuterScope) {
    EXPECT_EQ(run_int(R"(
fun make() {
    var x = 1;
    fun g() { var x = x + 1; return x; }
    return g();
}
return make();
)"),
              2);
}

// var x; 无初始化器 -> LOAD_NIL 填槽，后续读取合法（nil）。
TEST(CodeGen, VarNoInitializerReadsNil) {
    auto c = compile_only("fun f() { var x; var y = x; return y; }");
    ASSERT_TRUE(c.has_value()) << "var x; 后读取应合法（nil）";
}

TEST(CodeGen, VarNoInitializerRuntimeNil) {
    // 局部 var x; 使 x = nil（LOAD_NIL 填槽）；x == nil 成立 -> 返回 1。
    EXPECT_EQ(run_int("fun f() { var x; if (x == nil) { return 1; } return 0; } return f();"), 1);
}

// 嵌套 var 的初始化器值恰好压在 slot（值填槽，无 store/pop）。
TEST(CodeGen, VarInitValueFillsSlot) {
    EXPECT_EQ(run_int("fun f() { var x = 5; var y = x + 1; return y; } return f();"), 6);
}

// 嵌套具名 fun 绑定（declare + mark_init，无 store/pop）。
TEST(CodeGen, NestedNamedFun) {
    EXPECT_EQ(run_int("fun outer() { fun inner() { return 42; } return inner(); } return outer();"), 42);
}

TEST(CodeGen, ErrBreakOutsideLoop) {
    auto c = compile_only("break;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::BreakOutsideLoop);
}

// 循环上下文随函数隔离：嵌套函数内的 break 不绑定到外层循环（loop_stack_ 收入 FunctionCtx，
// 函数边界天然隔离）。
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
// 索引域 256 项,对齐 kMaxConstants 允许 65536 项)合法编译且运行正确--
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

// ============================================================
// 默认参数（印章方案：call_closure 垫充 + 序言身份判等换值）
// ============================================================

TEST(CodeGen, DefaultParamFillAndExplicitArg) {
    // 未传走默认;实参在位则实参胜出。
    EXPECT_EQ(run_int("fun add(a, b = 2) { return a + b; } return add(1);"), 3);
    EXPECT_EQ(run_int("fun add(a, b = 2) { return a + b; } return add(1, 10);"), 11);
}

TEST(CodeGen, DefaultParamLeftToRightFill) {
    // 连续缺省按声明序补齐:实参依次填靠前槽,余下槽走默认。
    EXPECT_EQ(run_int("fun f(a, b = 20, c = 30) { return a * 10000 + b * 100 + c; } return f(1);"), 12030);
    EXPECT_EQ(run_int("fun f(a, b = 20, c = 30) { return a * 10000 + b * 100 + c; } return f(1, 2);"), 10230);
    EXPECT_EQ(run_int("fun f(a, b = 20, c = 30) { return a * 10000 + b * 100 + c; } return f(1, 2, 3);"), 10203);
}

TEST(CodeGen, DefaultParamIndirectCall) {
    // 经变量(裸闭包)与经 bound method 间接调用,垫充路径与直接调用一致。
    EXPECT_EQ(run_int("fun f(a, b = 7) { return a + b; } var g = f; return g(1);"), 8);
    EXPECT_EQ(run_int("def C { m(a, b = 7) { return a + b; } } var o = C(); return o.m(1);"), 8);
    // 取出 bound method 后延迟调用:槽 0 = this 在位,缺省垫充同构适用。
    EXPECT_EQ(run_int("def C { m(a, b = 7) { return a + b; } } var o = C(); var f = o.m; return f(2);"), 9);
}

TEST(CodeGen, DefaultParamOnInit) {
    // init 缺省参数:call_class 换实例进槽 0 后走 call_closure,垫充同路。
    EXPECT_EQ(run_int("def P { init(x, y = 5) { this.x = x; this.y = y; } } var p = P(1); return p.x * 10 + p.y;"), 15);
    EXPECT_EQ(run_int("def P { init(x, y = 5) { this.x = x; this.y = y; } } var p = P(1, 2); return p.x * 10 + p.y;"),
              12);
}

TEST(CodeGen, DefaultParamLambdaAndNestedCapture) {
    // lambda 形参同用 compile_function,缺省发射一致。
    EXPECT_EQ(run_int("var f = fun(a, b = 2) { return a + b; }; return f(1);"), 3);
    // 嵌套函数经 upvalue 捕获参数不受自引用检查限制(调用时参数已在位)。
    EXPECT_EQ(run_int("fun f(a, b = (fun() { return a; })() + 1) { return a * 10 + b; } return f(3);"), 34);
}

TEST(CodeGen, DefaultParamSideEffectOnlyWhenMissing) {
    // 默认值表达式仅在实际未传时求值。
    auto out = run_source("var n = 0;"
                          "fun bump() { n = n + 1; return 100; }"
                          "fun f(a, b = bump()) { return b; }"
                          "var r1 = f(1, 9);" // b 实参在位,bump 不执行
                          "var v1 = n;"
                          "var r2 = f(1);" // b 未传,bump 执行
                          "var v2 = n;"
                          "return r1 * 1000 + v1 * 100 + r2 * 10 + v2;");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->as_int(), 10001); // r1=9, v1=0, r2=100, v2=1
}

TEST(CodeGen, DefaultParamFunctionArgNotMisjudged) {
    // 实参为原生函数值(与印章同类型)不误判未传:身份判等,函数值非印章。
    auto out = run_source("fun f(g = 1) { return g; }"
                          "var with_fn = f(len);"
                          "var with_default = f();"
                          "var r = 0;"
                          "if (with_fn === len) { r = r + 100; }" // 函数实参原样透传
                          "return r + with_default;");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->as_int(), 101);
}

TEST(CodeGen, DefaultParamOuterScopeStillReachable) {
    // 自引用检查仅限本函数参数槽:外层局部(经 upvalue)与全局照常可达。
    EXPECT_EQ(run_int("var v = 10; fun f(a = v) { return a; } return f();"), 10);
    EXPECT_EQ(run_int("fun outer() { var v = 10; fun inner(a = v) { return a; } return inner(); } return outer();"),
              10);
}

TEST(CodeGen, DefaultParamWrongArityRange) {
    // 低于 min_arity / 高于 arity 均报 WrongArity;有缺省参数报区间文案。
    auto lo = run_source("fun f(a, b = 2) { return b; } return f();");
    ASSERT_FALSE(lo.has_value());
    EXPECT_EQ(lo.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(lo.error().message().find("expects 1 to 2 args, got 0"), std::string::npos);

    auto hi = run_source("fun f(a, b = 2) { return b; } return f(1, 2, 3);");
    ASSERT_FALSE(hi.has_value());
    EXPECT_EQ(hi.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(hi.error().message().find("expects 1 to 2 args, got 3"), std::string::npos);
}

TEST(CodeGen, DefaultParamSingularArityMessageKept) {
    // 无缺省参数(min_arity == arity)保持单数文案,区间文案仅在真有缺省时出现。
    auto out = run_source("fun f(a) { return a; } return f();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::WrongArity);
    EXPECT_NE(out.error().message().find("expects 1 args, got 0"), std::string::npos);
}

TEST(CodeGen, DefaultParamFillKeepsSlotInvariantWithBodyLocals) {
    // 缺省序言求值路径的栈平衡回归:STORE_LOCAL 为 peek 不弹,须弹掉求值副本恢复
    // 「栈高 == 已填槽数」,否则体 var 声明值填槽错位(var 读到默认值残留)。
    EXPECT_EQ(run_int("fun f(n, m = n + 2) { var a = 100; var b = 7; return a + b + n + m; } return f(1);"), 111);
    EXPECT_EQ(run_int("fun f(n, m = n + 2) { var a = 100; var b = 7; return a + b + n + m; } return f(1, 2);"), 110);
}

// 序言形态:逐缺省槽 LOAD_LOCAL -> LOAD_REG DefaultMark -> EQUAL -> JUMP_FALSE -> 默认值
// 表达式 -> STORE_LOCAL。全为既有指令,栈形平衡(序言后栈空)。
TEST(CodeGen, DefaultParamPrologueDisassembly) {
    auto c = compile_only("fun f(a, b = 5) { return b; } return f(1);");
    ASSERT_TRUE(c.has_value()) << c.error().message();
    // 序言发射在子函数 f 的 unit 内(入口 unit 只有 CLOSURE/调用序列):经入口常量池取 f 的
    // ObjFunction 再反汇编其 unit。
    const ObjFunction* f = nullptr;
    for (const auto& v: c->unit().constants) {
        if (v.is_obj() && aria::Object::is<ObjFunction>(v.as_obj())) {
            f = aria::Object::as<ObjFunction>(v.as_obj());
        }
    }
    ASSERT_NE(f, nullptr);
    EXPECT_EQ(f->arity(), 2);
    EXPECT_EQ(f->min_arity(), 1); // 必传 a,缺省 b
    const auto text = f->unit().disassemble("f");
    EXPECT_NE(text.find("LOAD_LOCAL"), aria::String::npos);
    EXPECT_NE(text.find("LOAD_REG"), aria::String::npos);
    EXPECT_NE(text.find("DefaultMark"), aria::String::npos); // 寄存器可读名入反汇编注释
    EXPECT_NE(text.find("EQUAL"), aria::String::npos);
    EXPECT_NE(text.find("JUMP_FALSE"), aria::String::npos);
    EXPECT_NE(text.find("STORE_LOCAL"), aria::String::npos);
}

TEST(CodeGen, DefaultParamReferencesEarlierParam) {
    // 前序参数在缺省表达式中可引用:序言从左到右求值,轮到本槽时前序槽必已就位。
    EXPECT_EQ(run_int("fun f(a, b = a * 2) { return b; } return f(21);"), 42);
    // 链式缺省:后一个缺省引用前一个缺省参数。
    EXPECT_EQ(run_int("fun f(a = 1, b = a * 2, c = b + 1) { return a * 100 + b * 10 + c; } return f();"), 123);
    EXPECT_EQ(run_int("fun f(a = 1, b = a * 2, c = b + 1) { return a * 100 + b * 10 + c; } return f(5);"), 611);
}

TEST(CodeGen, DefaultParamMutationOfEarlierParam) {
    // 缺省表达式内对前序参数赋值合法(普通局部语义),表达式值为所赋值。
    EXPECT_EQ(run_int("fun f(a, b = (a = 1)) { return a * 10 + b; } return f(5);"), 11);
}

TEST(CodeGen, DefaultParamUnregisteredFallsToGlobal) {
    // 自身/后序参数未登记,名字按常规解析链落外层/全局(Python/C++ 默认值作用域同款):
    // 无同名全局 -> 缺省被求值时 UndefinedVariable;有同名全局 -> 用全局值(不指向参数)。
    auto out = run_source("fun f(a = b, b = 2) { return b; } return f();");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
    EXPECT_EQ(run_int("var b = 9; fun f(a = b, b = 2) { return a * 10 + b; } return f();"), 92);
    EXPECT_EQ(run_int("var a = 7; fun f(a = a) { return a; } return f();"), 7);
}

TEST(CodeGen, VarargsCompiles) {
    // varargs 已落地(list 载体):编译通过即可(行为面由 Compiler.Varargs* 端到端覆盖)。
    auto c = compile_only("fun f(a, b = 2, ...rest) { return a; }");
    EXPECT_TRUE(c.has_value());
}

// 出错即停 + ~ModuleCtx 沿 enclosing_ 链释放：lambda（表达式位）体内 break 触发 BreakOutsideLoop
// （lambda 自身 loop_stack 空，不绑外层 for），fail() 抛 AriaCompileException 直接 unwind（游标停在
// lambda 上下文、不还原），~ModuleCtx 沿 enclosing_ 链（lambda -> entry）逐个释放。
// 验证不崩溃且返 BreakOutsideLoop。
TEST(CodeGen, ErrStopOnNestedLambdaInExpr) {
    auto c = compile_only("for (var i = 0; i < 3; i = i + 1) { var x = 1 + (fun() { break; })(); }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::BreakOutsideLoop);
}

// ============================================================
// match（纯降糖：subject 驻栈 + DUP 逐臂比较 + 共享 MatchNoArm 兜底异常）
// ============================================================

TEST(CodeGen, MatchStmtFirstArmHit) {
    EXPECT_EQ(run_int("var r = 0; match (1) { 1 => r = 1; 2 => r = 2; _ => r = 3; } return r;"), 1);
}

TEST(CodeGen, MatchStmtWildcardFallback) {
    EXPECT_EQ(run_int("var r = 0; match (99) { 1 => r = 1; _ => r = 2; } return r;"), 2);
}

TEST(CodeGen, MatchStmtBlockArm) {
    // 块臂（多语句 statement）与语句臂同链；块臂净零值不破栈。
    EXPECT_EQ(run_int("var r = 0; match (2) { 1 => r = 1; 2 => { r = 2; r = r + 10; } _ => r = 3; } return r;"), 12);
}

TEST(CodeGen, MatchStmtNoArmThrowsMatchNoArm) {
    auto out = run_source("match (99) { 1 => 2; }");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::MatchNoArm);
    EXPECT_NE(out.error().message().find("no arm matched"), std::string::npos);
}

TEST(CodeGen, MatchNoArmCatchableAndShared) {
    // 兜底异常是寄存器里的共享单例:可被 try/catch 捕获(catch 绑原值),两次抛出身份恒一。
    auto out = run_source(R"(
        var e1 = nil;
        var e2 = nil;
        try { match (9) { 1 => 2; } } catch (e) { e1 = e; }
        try { match (8) { 1 => 2; } } catch (e) { e2 = e; }
        if (e1 === e2) { return 7; }
        return 0;
    )");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->as_int(), 7);
}

TEST(CodeGen, MatchExprTakesValue) { EXPECT_EQ(run_int("return match (2) { 1 => 10, 2 => 20, _ => 30 };"), 20); }

TEST(CodeGen, MatchExprInVarInitializer) {
    // 值填槽窗口:降糖不登记任何局部(无隐藏临时),var 初始化器内槽位无错位;臂体读外层局部。
    EXPECT_EQ(run_int("fun f() { var a = 1; var x = match (a) { 1 => a + 10, _ => 0 }; return x; } return f();"), 11);
}

TEST(CodeGen, MatchExprAsCallArg) {
    // 表达式位置嵌套在运算实参窗口,subject 驻栈与外层运算值共存。
    EXPECT_EQ(run_int("return 1 + match (2) { 2 => 3, _ => 4 };"), 4);
}

TEST(CodeGen, MatchSubjectEvaluatedOnce) {
    // subject 求值恰一次(DUP 副本跨臂比较);同模式双臂首中即停。
    EXPECT_EQ(run_int(R"(
        var calls = 0;
        fun bump() { calls = calls + 1; return 7; }
        var v = 0;
        match (bump()) { 1 => v = 1; 7 => v = 2; 7 => v = 4; _ => v = 3; }
        return calls * 10 + v;
    )"),
              12);
}

TEST(CodeGen, MatchPatternLazyShortCircuit) {
    // 惰性:命中臂之后的模式不求值(calls 恒 0);未命中臂的模式恰求值一次。
    EXPECT_EQ(run_int(R"(
        var calls = 0;
        fun p() { calls = calls + 1; return 5; }
        var v = 0;
        match (1) { 1 => v = 1; p() => v = 2; _ => v = 3; }
        return calls * 10 + v;
    )"),
              1);
    EXPECT_EQ(run_int(R"(
        var calls = 0;
        fun p() { calls = calls + 1; return 5; }
        var v = 0;
        match (3) { 1 => v = 1; p() => v = 2; 3 => v = 3; _ => v = 4; }
        return calls * 10 + v;
    )"),
              13);
}

TEST(CodeGen, ErrUnreachableArmAfterWildcard) {
    // 通配臂恒末臂:其后臂任何输入下不可达,编译期拒绝(静默截断会吞臂序 bug);双通配同理。
    auto s = compile_only("match (1) { _ => 2; 3 => 4; }");
    ASSERT_FALSE(s.has_value());
    EXPECT_EQ(s.error().code(), ErrorCode::UnreachableArm);
    auto d = compile_only("match (1) { _ => 2; _ => 3; }");
    ASSERT_FALSE(d.has_value());
    EXPECT_EQ(d.error().code(), ErrorCode::UnreachableArm);
    auto e = compile_only("return match (1) { _ => 2, 3 => 4 };");
    ASSERT_FALSE(e.has_value());
    EXPECT_EQ(e.error().code(), ErrorCode::UnreachableArm);
}

TEST(CodeGen, MatchDisassembly) {
    auto compiled = compile_only(R"(
        match (1) { 1 => print 1; 2 => print 2; _ => print 3; }
    )");
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message();
    const auto text = compiled->unit().disassemble("<test>");
    // 逐臂链形态:每臂 DUP -> EQUAL -> JUMP_FALSE(未命中) -> POP(丢弃 subject) -> 臂体;
    // 兜底 LOAD_REG MatchNoArm -> THROW 相邻。
    EXPECT_TRUE(lines_adjacent(text, "EQUAL", "JUMP_FALSE"));
    EXPECT_TRUE(lines_adjacent(text, "JUMP_FALSE", "POP"));
    EXPECT_TRUE(lines_adjacent(text, "LOAD_REG", "THROW"));
    EXPECT_NE(text.find("MatchNoArm"), aria::String::npos);
    // 链先于兜底,兜底先于 L_end 汇合。
    EXPECT_LT(text.find("DUP"), text.find("LOAD_REG"));
    EXPECT_LT(text.find("JUMP_FALSE"), text.find("LOAD_REG"));
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
    EXPECT_EQ(aria::format_value((*run_source("return type(42);"))), "Int");
    EXPECT_EQ(aria::format_value((*run_source("return type(true);"))), "Bool");
    EXPECT_EQ(aria::format_value((*run_source("return type(nil);"))), "Nil");
    EXPECT_EQ(aria::format_value((*run_source("return type(\"x\");"))), "String");
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
    EXPECT_EQ(aria::format_value((*run_source("return str(nil);"))), "nil");
    EXPECT_EQ(aria::format_value((*run_source("return str(42);"))), "42");
    EXPECT_EQ(aria::format_value((*run_source("return str(true);"))), "true");
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
    // 用户顶层 var 同名覆盖内置：DEF_GLOBAL 在运行期 set 覆写同名全局，内置被替换。
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
    // assert 自检 argc：assert() 0 参 / assert(1,2,3) 3 参 -> WrongArity。
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
    // VM 级只读 builtins_ 表不随 run() 重新注入，shadow 跨行持久。stress GC 锻炼 builtins_ 根接线。
    auto  vm = std::make_unique<AriaVM>();
    auto& gc = vm->gc();
    gc.set_stress(true);
    auto module = new_module(gc, "<test>"); // StringView 重载:名字经工厂内部 intern 并自守

    auto c1 = compile_source(gc, module, "var len = 5;");
    ASSERT_TRUE(c1.has_value());
    auto r1 = vm->run(*c1);
    ASSERT_TRUE(r1.has_value()) << r1.error().message();

    auto c2 = compile_source(gc, module, "return len;");
    ASSERT_TRUE(c2.has_value());
    auto r2 = vm->run(*c2);
    ASSERT_TRUE(r2.has_value()) << r2.error().message();
    EXPECT_EQ(r2->as_int(), 5); // 非内置 <fn len>
}

TEST(CodeGen, BuiltinBareAssignWithoutVarFails) {
    // 裸名赋值 `len = 5`（无 var 声明）：模块 globals 未命中 -> UndefinedVariable，不回退 builtins
    // 写（STORE_GLOBAL 不回退，与 grammar §205-206「赋值不隐式创建、必须先 var 声明」一致）。
    auto out = run_source("len = 5; return len;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedVariable);
}

// ============================================================
// M3 异常（try/catch/throw）
// ============================================================
// 统一异常通道：aria throw 与 VM 运行时错误都走挂起寄存器 + unwind 查 CodeUnit 异常记录表。
// 单寄存器模型：用户 throw 存原值保类型、运行时错误装箱 ObjException 携码；未捕获物化 Error
// 并烘焙逐帧堆栈跟踪（外 -> 内）。run_source 的 stress GC 默认开，锻炼 pending_error_ 根接线
// （pitfalls 坑 #8）。合成模块 <test> 的位置前缀退化 "<test>:line"。

TEST(CodeGen, ThrowIntCaughtBindsValue) {
    // throw 42 被 catch 捕获，e 绑原值保类型（单寄存器模型，坑 #7）。
    EXPECT_EQ(run_int("try { throw 42; } catch (e) { return e; }"), 42);
}

TEST(CodeGen, ThrowStringCaughtBindsValue) {
    auto out = run_source("try { throw \"boom\"; } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    // 渲染即串内容，证绑的是 ObjString 原值（非消息串）。
    EXPECT_EQ(aria::format_value(*out), "boom");
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
    // 运行时错误（除零）可捕获：e 绑 ObjException（携码 + 完整烘焙消息，print/str 渲染之；
    // 消息不含位置前缀，同 Python str(e)，位置只在未捕获出口的 at 跟踪行给出）。
    auto out = run_source("try { return 1 / 0; } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    ASSERT_TRUE(out->is_obj());
    const auto ex = aria::Object::as<aria::ObjException>(out->as_obj());
    ASSERT_NE(ex, nullptr);
    EXPECT_EQ(ex->code(), ErrorCode::DivisionByZero);
    EXPECT_EQ(ex->to_string(), "Runtime: DivisionByZero integer division by zero");
}

TEST(CodeGen, RethrowPreservesCode) {
    // re-throw 保码（单寄存器收益，坑 #7）：catch 绑 ObjException 再 throw（catch 体不在
    // 本层受保护区间内，向外传播），未捕获经 from_baked 回 DivisionByZero（非 UncaughtException）。
    auto out = run_source("try { return 1 / 0; } catch (e) { throw e; }");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::DivisionByZero);
    EXPECT_EQ(out.error().message(), "Runtime: DivisionByZero integer division by zero\n"
                                     "  at <main> (<test>:1)");
}

TEST(CodeGen, NativeFailCaughtByTry) {
    // 原生报错（len 非 String）同走异常通道：vm.fail 装箱 ObjException 入寄存器，CALL 失败
    // 经 unwind 被捕获；str(e) 渲染完整消息（无位置前缀，位置只在未捕获跟踪行给出；
    // 原生不进帧时即 CALL 站点行，坑 #15）。
    auto out = run_source("try { return len(nil); } catch (e) { return str(e); }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(aria::format_value(*out), "Runtime: TypeMismatch len requires a string or list, got Nil");
}

TEST(CodeGen, NestedTryInnerCatches) {
    // 嵌套 try：内层捕获（find_try_handler 取最内层覆盖区间）。
    auto out = run_source("try { try { throw 1; } catch (i) { return i; } } catch (o) { return 2; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->as_int(), 1);
}

TEST(CodeGen, NestedTryOuterCatchesInnerRethrow) {
    // 内层 catch re-throw：THROW 指令在 catch 体（本层区间之外、外层区间之内）-> 外层捕获。
    auto out = run_source("try { try { throw 1; } catch (i) { throw i; } } catch (o) { return o + 10; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(out->as_int(), 11);
}

TEST(CodeGen, CrossFrameCatch) {
    // 跨帧捕获：被调函数 throw，unwind 逐帧 exit_frame 后在调用者帧命中 handler（坑 #13），
    // 截值栈到 frame.slots + stack_depth，异常值 push 落 catch 参数槽。
    auto out = run_source("fun f() { throw \"cross\"; } try { f(); } catch (e) { return e; }");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(aria::format_value(*out), "cross");
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
    EXPECT_EQ(out->as_int(), 7);
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
    EXPECT_EQ(out->as_int(), 8);
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
    // 消息首行无位置前缀，错误位置 = 最内 at 行（div 的除法行）。
    EXPECT_EQ(out.error().message(), "Runtime: DivisionByZero integer division by zero\n"
                                     "  at <main> (<test>:8)\n"
                                     "  at mid (<test>:6)\n"
                                     "  at div (<test>:3)");
}

TEST(CodeGen, FinallyIsPlainIdentifierAfterRemoval) {
    // finally 已裁撤：不再是关键字，回归普通标识符可绑定。
    auto c = compile_only("var finally = 1; print finally;");
    ASSERT_TRUE(c.has_value()) << c.error().message();
}

TEST(CodeGen, TryCatchEmitsTryRecordAndThrow) {
    // 发射核对：try_records 一条，受保护区间 [begin, end) 覆盖 try 体（THROW 在内），
    // handle 指向跳过 catch 的 JUMP 之后，stack_depth = try 入口局部数（<main> 顶层仅
    // slot 0 哑元 = 1）；反汇编出现 try records 小节（非空才列）。
    auto c = compile_only("try { throw 1; } catch (e) { print e; }");
    ASSERT_TRUE(c.has_value()) << c.error().message();
    const auto& cu = c->unit();
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
    const auto& recs = c->unit().try_records;
    ASSERT_EQ(recs.size(), 2u);
    EXPECT_LT(recs[0].begin, recs[1].begin);
    EXPECT_LT(recs[1].begin, recs[0].end); // 内层起点在外层区间内
    EXPECT_LT(recs[1].end, recs[0].end);
}

// ============================================================
// M5 类与对象（阶段 3 编译翻转）
// ============================================================

// 空类 + 隐式 Object 根继承：无成员无 init，实例化走 bootstrap 的原生 no-op init。
TEST(CodeGen, EmptyDefInstantiates) { EXPECT_EQ(run_int("def X { } var x = X(); return 1;"), 1); }

// def 体内 throw（成员初始化器经 lambda 调用抛出）：半成品类随 unwind 截栈丢弃、类名从未
// 绑定（TryRecord.stack_depth 记在 def 语句前，异常白赚语义）。
TEST(CodeGen, DefThrowLeavesClassUnbound) {
    EXPECT_EQ(run_int(R"(
var made = 0;
try {
    def Boom { var x = (fun() { throw "boom"; })(); }
    made = 1;
} catch (e) { }
var bound = 0;
try { var probe = Boom; bound = 1; } catch (e2) { }
return made * 10 + bound;
)"),
              0);
}

// superclass 运行期解析：非类值 -> MAKE_CLASS 报 TypeMismatch（编译期不查全局，未命中沿用
// 运行期 UndefinedVariable）。
TEST(CodeGen, SuperclassNotAClassIsRuntimeError) {
    auto out = run_source("var B = 5; def F : B { } return 1;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

// ---- 编译期错误 ----

// this 在类外（普通函数 / 顶层 / 静态方法）：沿 ctx 链无实例方法，永不落全局。
TEST(CodeGen, ErrThisOutsideClassInFunction) {
    auto c = compile_only("fun f() { return this; }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::ThisOutsideClass);
}

TEST(CodeGen, ErrThisOutsideClassAtTopLevel) {
    auto c = compile_only("var x = this;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::ThisOutsideClass);
}

TEST(CodeGen, ErrThisOutsideClassInStaticMethod) {
    auto c = compile_only("def F { fun s() { return this; } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::ThisOutsideClass);
}

// super 编译期禁于非直接方法帧（2026-09-14 拍板）：顶层 fun / 静态方法 / 实例方法内嵌套
// lambda / super.x 读取形态同判据（visitSuperExprNode 单点检查）。
TEST(CodeGen, ErrSuperOutsideMethodInFunction) {
    auto c = compile_only("fun f() { return super.m(); }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::SuperOutsideMethod);
}

TEST(CodeGen, ErrSuperOutsideMethodInStaticMethod) {
    auto c = compile_only("def F { fun s() { return super.m(); } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::SuperOutsideMethod);
}

TEST(CodeGen, ErrSuperOutsideMethodInNestedLambda) {
    auto c = compile_only("def F { m() { return (fun() { return super.m(); })(); } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::SuperOutsideMethod);
}

TEST(CodeGen, ErrSuperFieldReadOutsideMethod) {
    auto c = compile_only("var y = super.x;");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::SuperOutsideMethod);
}

// super 写形态非左值 -> InvalidAssignmentTarget（裸 super 已是解析错误，见 test_parser）。
TEST(CodeGen, ErrSuperFieldStoreInvalidTarget) {
    auto c = compile_only("def F { m() { super.x = 1; } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::InvalidAssignmentTarget);
}

TEST(CodeGen, ErrSuperCompoundInvalidTarget) {
    auto c = compile_only("def F { m() { super.x += 1; } }");
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::InvalidAssignmentTarget);
}

// ---- 字段访问 / 方法调用（visitFieldAccessNode 三模式 + visitCallNode 通用路径）----

// 路线表验收样例 1：类定义 + 实例化（init 带参、this 字段读写、方法调用）。
TEST(CodeGen, ClassInstantiateAndMethodCall) {
    EXPECT_EQ(run_int(R"(
def Point {
    init(x, y) { this.x = x; this.y = y; }
    sum() { return this.x + this.y; }
}
var p = Point(3, 4);
return p.sum();
)"),
              7);
}

// 路线表验收样例 2/3：继承 + super（init 经构造函数沿链派生、覆写后 super.get() 调父实现）。
TEST(CodeGen, InheritanceAndSuperCall) {
    EXPECT_EQ(run_int(R"(
def Base {
    init() { this.v = 1; }
    get() { return this.v; }
}
def Sub : Base {
    get() { return super.get() + 10; }
}
var s = Sub();
return s.get();
)"),
              11);
}

// 路线表验收样例 4：静态成员（var 静态 eager 求值 + fun 静态方法 + 类上赋值原槽更新）。
TEST(CodeGen, StaticVarAndStaticMethod) {
    EXPECT_EQ(run_int(R"(
def Counter {
    var count = 10;
    fun get() { return Counter.count; }
}
var a = Counter.count;
Counter.count = 20;
var b = Counter.get();
return a + b;
)"),
              30);
}

// 类成员读穿透 / 写遮蔽：Sub.tag 读沿链命中 Base，赋值落 Sub 自身表，Base 不变。
TEST(CodeGen, StaticReadThroughWriteShadow) {
    EXPECT_EQ(run_int(R"(
def Base { var tag = 1; }
def Sub : Base { }
var a = Sub.tag;
Sub.tag = 2;
return a * 100 + Sub.tag * 10 + Base.tag;
)"),
              121);
}

// 实例字段遮蔽类静态：fields 命中优先，类表不受实例赋值影响。
TEST(CodeGen, InstanceFieldShadowsStatic) {
    EXPECT_EQ(run_int(R"(
def F {
    var v = 5;
    get() { return this.v; }
}
var f = F();
f.v = 7;
return f.v + F.v + f.get();
)"),
              19);
}

// this 嵌套捕获（arrow 语义）：lambda 内 this.x 经 upvalue 读写仍落原实例（退化路径
// LOAD_UPVALUE this + LOAD/STORE_FIELD）。
TEST(CodeGen, ThisNestedCapture) {
    EXPECT_EQ(run_int(R"(
def P {
    init(x) { this.x = x; }
    bump() { (fun() { this.x = this.x + 1; })(); return this.x; }
}
var p = P(1);
p.bump();
return p.bump();
)"),
              3);
}

// 方法返回后经 this upvalue 延迟调用：帧已退、upvalue 已 close 迁出，闭包仍绑原实例。
TEST(CodeGen, DelayedCallViaThisUpvalue) {
    EXPECT_EQ(run_int(R"(
def Box {
    init(v) { this.v = v; }
    binder() { return fun() { return this.v; }; }
}
var b = Box(42);
var delayed = b.binder();
return delayed();
)"),
              42);
}

// super 不污染动态派发（bound 缓存铁则 2）：super.m 绑父实现后，s.m 仍派发子类覆写。
TEST(CodeGen, SuperDoesNotPolluteDynamicDispatch) {
    EXPECT_EQ(run_int(R"(
def Base { m() { return 1; } }
def Sub : Base {
    m() { return 10 + super.m(); }
    callSuper() { return super.m(); }
}
var s = Sub();
return s.callSuper() + s.m();
)"),
              12);
}

// 动态新增（2026-09-11 改定）：类上 / 实例上赋新名成员均落接收方自身表。
TEST(CodeGen, DynamicMemberAdd) {
    EXPECT_EQ(run_int(R"(
def F { }
F.extra = 9;
var f = F();
f.field = 4;
return F.extra + f.field;
)"),
              13);
}

// 嵌套函数内 super 禁（2026-09-14 拍板）的等价写法：先取后用（super.m 取 bound method 值，
// 闭包内延迟调用）。
TEST(CodeGen, SuperMethodTakeThenUse) {
    EXPECT_EQ(run_int(R"(
def Base { m() { return 5; } }
def Sub : Base {
    run() {
        var mth = super.m;
        var g = fun() { return mth(); };
        return g();
    }
}
return Sub().run();
)"),
              5);
}

// 函数内 def：类值填槽绑局部，返回类后照常实例化。
TEST(CodeGen, DefInFunctionBindsLocal) {
    EXPECT_EQ(run_int(R"(
fun make() {
    def Inner { m() { return 3; } }
    return Inner;
}
var K = make();
return K().m();
)"),
              3);
}

// M5 落地后用户定义类即可迭代：iter/has_next/next 经 LOAD_FIELD 返 bound method + CALL
// （内建 list/map/string 迭代仍待容器里程碑）。
TEST(CodeGen, UserClassForIn) {
    EXPECT_EQ(run_int(R"(
def Three {
    init() { this.i = 0; }
    iter() { return this; }
    has_next() { return this.i < 3; }
    next() { this.i = this.i + 1; return this.i - 1; }
}
var sum = 0;
for (x in Three()) { sum = sum + x; }
return sum;
)"),
              3);
}

// 字段访问运行期错误：原语类型不支持字段访问（nil 与原语同走 UndefinedProperty 统一文案）。
TEST(CodeGen, FieldAccessOnPrimitiveIsRuntimeError) {
    auto out = run_source("var a = 1; return a.x;");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::UndefinedProperty);
}

// ---- 复合赋值 / 前置自增的字段腿（Locate 启用，compound-assignment-lowering.md §4.2）----

// 实例字段复合赋值与前置自增：<obj> DUP LOAD_FIELD ... STORE_FIELD，locator 单次求值。
TEST(CodeGen, CompoundAssignOnInstanceField) {
    EXPECT_EQ(run_int(R"(
def C { init() { this.n = 10; } }
var c = C();
c.n += 5;
++c.n;
return c.n;
)"),
              16);
}

// this 字段复合赋值：定位腿折叠 THIS_FIELD Load 形（locator = this 槽位，DUP 副本无人消费故不发）。
TEST(CodeGen, CompoundAssignOnThisField) {
    EXPECT_EQ(run_int(R"(
def C2 {
    init() { this.n = 7; }
    bump() { this.n += 3; ++this.n; return this.n; }
}
var c2 = C2();
return c2.bump();
)"),
              11);
}

// this 字段复合赋值作非末位实参：定位腿折叠 THIS_FIELD Load 形（无 DUP 副本滞留），栈形不偏移——
// 副本滞留会把 callee 槽顶成 Int（CallNonCallable）或把前面实参顶错位。
TEST(CodeGen, ThisFieldCompoundAsCallArg) {
    EXPECT_EQ(run_int(R"(
fun h(a, b) { return a * 100 + b; }
def C {
    init() { this.n = 7; }
    bump() { return h(1, this.n += 3); }
}
var c = C();
return c.bump();
)"),
              110);
}

// 类静态字段复合赋值：locator = 类名（全局重解析），读穿透 / 写遮蔽与普通赋值同路。
TEST(CodeGen, CompoundAssignOnStaticField) {
    EXPECT_EQ(run_int(R"(
def S { var v = 100; }
S.v += 1;
return S.v;
)"),
              101);
}

// locator 单次求值：复合赋值字段腿的接收者表达式（带副作用的调用）只跑一次。
TEST(CodeGen, CompoundFieldLocatorEvaluatedOnce) {
    EXPECT_EQ(run_int(R"(
var calls = 0;
def C { init() { this.n = 100; } }
var holder = C();
fun get() { calls = calls + 1; return holder; }
get().n += 5;
return calls * 1000 + holder.n;
)"),
              1105);
}

// 成员即表写入（RedefinedMember 退役）：重名后写遮蔽，类静态读回取后值。
TEST(CodeGen, MemberDuplicateShadowsPrevious) {
    EXPECT_EQ(run_int(R"(
def C { var v = 1; var v = 2; }
return C.v;
)"),
              2);
}

// ---- 反汇编形状 ----

// def 反汇编：LOAD_REG ObjectClass -> MAKE_CLASS；成员序与源序一致（静态 var 先于方法）；方法闭包
// CLOSURE 与 MAKE_METHOD/MAKE_STATIC 相邻；类绑定 DEF_GLOBAL 恰一次（方法体在各自 unit，
// 无 DEF_GLOBAL 混入成员发射）。
TEST(CodeGen, DefDisassembly) {
    auto compiled = compile_only(R"(
def Animal {
    var kind = 1;
    fun static_get() { return Animal.kind; }
    init(name) { this.name = name; }
    speak() { return 2; }
}
)");
    ASSERT_TRUE(compiled.has_value()) << compiled.error().message();
    const auto text = compiled->unit().disassemble("<test>");
    ASSERT_NE(text.find("LOAD_REG"), aria::String::npos);
    ASSERT_NE(text.find("MAKE_CLASS"), aria::String::npos);
    EXPECT_LT(text.find("LOAD_REG"), text.find("MAKE_CLASS"));
    // 成员序：静态 var（MAKE_STATIC）先于首个方法闭包（CLOSURE）。
    ASSERT_NE(text.find("MAKE_STATIC"), aria::String::npos);
    ASSERT_NE(text.find("CLOSURE"), aria::String::npos);
    EXPECT_LT(text.find("MAKE_STATIC"), text.find("CLOSURE"));
    // 方法闭包与注册指令相邻（每指令一行）。
    EXPECT_TRUE(lines_adjacent(text, "CLOSURE", "MAKE_STATIC")); // static_get（fun 静态）
    EXPECT_TRUE(lines_adjacent(text, "CLOSURE", "MAKE_METHOD")); // init / speak（实例方法）
    // 类绑定 DEF_GLOBAL 恰一次。
    aria::usize def_global_count = 0;
    for (aria::usize pos = 0; (pos = text.find("DEF_GLOBAL", pos)) != aria::String::npos; pos += 10) {
        ++def_global_count;
    }
    EXPECT_EQ(def_global_count, 1u);
}

// ============================================================
// list 字面量(值表示 + MAKE_LIST + len/str/print)
// ============================================================

// 字面量求值:元素按序求值、恰好各一次(经全局计数器观察副作用),渲染保序。
TEST(CodeGen, ListLiteralElementsEvaluatedInOrder) {
    EXPECT_EQ(run_int(R"(
var n = 0;
fun f() { n = n + 1; return n * 10; }
var xs = [f(), f(), f()];
if (n != 3) { return -1; }
if (str(xs) != "[10, 20, 30]") { return -2; }
return n;
)"),
              3);
}

// 空字面量与嵌套:嵌套字符串走 debug 形(带引号),嵌套 list 递归渲染。
TEST(CodeGen, ListLiteralEmptyAndNested) {
    EXPECT_EQ(aria::format_value(*run_source("return [];")), "[]");
    EXPECT_EQ(aria::format_value(*run_source(R"(return [1, [2, "ab"], nil];)")), "[1, [2, \"ab\"], nil]");
}

// len:list 元素数;str/print 走同一渲染位。
TEST(CodeGen, ListLen) {
    EXPECT_EQ(run_int("return len([]);"), 0);
    EXPECT_EQ(run_int("return len([10, 20, 30]);"), 3);
    EXPECT_EQ(run_int("return len([[], [1]]);"), 2);
}

// len 类型面:非 string/list 报 TypeMismatch(运行期)。
TEST(CodeGen, ListLenTypeMismatch) {
    auto out = run_source("return len(1);");
    ASSERT_FALSE(out.has_value());
    EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch);
}

// == 按内容递归;=== 恒指针;自比较 == 快速路径。
TEST(CodeGen, ListEqualityContentVsIdentity) {
    EXPECT_TRUE((*run_source("return [1, [2]] == [1, [2]];")).as_bool()); // 嵌套递归
    EXPECT_FALSE((*run_source("return [1] == [2];")).as_bool());
    EXPECT_FALSE((*run_source("return [1, 2] == [1, 2, 3];")).as_bool()); // 长度不等
    EXPECT_FALSE((*run_source(R"(return [1] === [1];)")).as_bool());      // 两字面量两对象
    EXPECT_TRUE((*run_source("var xs = [1]; return xs == xs;")).as_bool());
    EXPECT_TRUE((*run_source("var xs = [1]; return xs === xs;")).as_bool());
    EXPECT_FALSE((*run_source("return [1] == \"1\";")).as_bool()); // 跨类型
}

// list 作一等值:实参传递、经变量返回、跨 GC 点存活(run_source 开 stress GC,
// 元素串经 MAKE_LIST「栈即根」+ ObjList::trace 级联保命)。
TEST(CodeGen, ListPassingAndGcStress) {
    EXPECT_EQ(aria::format_value(*run_source(R"(
fun echo(xs) { return xs; }
var kept = ["aaa", ["bbb", "ccc"]];
return echo(kept);
)")),
              "[\"aaa\", [\"bbb\", \"ccc\"]]");
}

// 元素数超容量(65536 个,MAKE_LIST 操作数 u16 上限 65535 之外) -> TooManyElements
//(先检后发:emit 前即报,不先发 6 万多个元素表达式)。
TEST(CodeGen, ErrTooManyElements) {
    std::string src = "[";
    for (int i = 0; i < 65535; ++i) {
        src += "1,";
    }
    src += "1];"; // 65535 + 1 = 65536 个元素
    auto c = compile_only(src);
    ASSERT_FALSE(c.has_value());
    EXPECT_EQ(c.error().code(), ErrorCode::TooManyElements);
}

// 容量下界钉子:恰 65535 个元素(MAKE_LIST 操作数 u16 上限位置)合法编译且运行正确 --
// 勿把边界「修正」为 65534(那会白禁合法操作数 65535)。
TEST(CodeGen, ExactlyMaxListElementsCompiles) {
    std::string src = "return len([";
    for (int i = 0; i < 65534; ++i) {
        src += "1,";
    }
    src += "1]);"; // 65534 + 1 = 65535 个元素
    EXPECT_EQ(run_int(src), 65535);
}

// ---- 下标读写(LOAD_INDEX/STORE_INDEX + 下标四模式 lowering) ----

// 读:常量键/变量键/嵌套链式。
TEST(CodeGen, IndexRead) {
    EXPECT_EQ(run_int("return [10, 20, 30][1];"), 20);
    EXPECT_EQ(run_int("return [[1, 2], [3, 4]][1][0];"), 3);
    EXPECT_EQ(run_int("var xs = [5, 6]; var i = 1; return xs[i];"), 6);
}

// 写:普通 = 三腿(Prepare 备 obj+idx 对 -> 值 -> Store)。
TEST(CodeGen, IndexWrite) {
    EXPECT_EQ(run_int(R"(
var xs = [1, 2];
xs[0] = 9;
xs[2 - 1] = xs[0] + 1;
return xs[0] + xs[1];
)"),
              19);
}

// STORE_INDEX peek-store 留值:下标赋值作为表达式,值为所存值。
TEST(CodeGen, IndexAssignExpressionValue) { EXPECT_EQ(run_int("var xs = [1]; return (xs[0] = 5) + xs[0];"), 10); }

// 复合赋值与前置自增:Locate 腿 DUP2 保 (obj, idx) 对,Store 腿复用。
TEST(CodeGen, CompoundAssignOnIndex) {
    EXPECT_EQ(run_int(R"(
var xs = [1, 2];
xs[0] += 5;
xs[1] *= 3;
return xs[0] + xs[1];
)"),
              12);
    EXPECT_EQ(run_int("var xs = [5]; ++xs[0]; return xs[0];"), 6);
}

// locator-once:下标表达式 f() 在复合赋值中只求值一次(单调性经全局计数器观察)。
TEST(CodeGen, CompoundIndexLocatorEvaluatedOnce) {
    EXPECT_EQ(run_int(R"(
var xs = [1];
var n = 0;
fun f() { n = n + 1; return 0; }
xs[f()] += 1;
return n * 100 + xs[0];
)"),
              102);
}

// 越界:读/负数/写都报 IndexOutOfBounds(运行期,可 catch)。
TEST(CodeGen, ErrIndexOutOfBounds) {
    auto read = run_source("return [1][5];");
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code(), ErrorCode::IndexOutOfBounds);
    auto negative = run_source("return [1, 2][0 - 1];");
    ASSERT_FALSE(negative.has_value());
    EXPECT_EQ(negative.error().code(), ErrorCode::IndexOutOfBounds);
    auto store = run_source("var xs = [1]; xs[3] = 1; return 0;");
    ASSERT_FALSE(store.has_value());
    EXPECT_EQ(store.error().code(), ErrorCode::IndexOutOfBounds);
}

// 键类型:非整数键(f64/nil/bool)与非下标类型(原语)都报 TypeMismatch。
TEST(CodeGen, ErrIndexKeyTypeMismatch) {
    for (const std::string_view src:
         {"return [1][1.5];", "return [1][nil];", "return [1][true];", "var x = 1; return x[0];"}) {
        auto out = run_source(src);
        ASSERT_FALSE(out.has_value()) << src;
        EXPECT_EQ(out.error().code(), ErrorCode::TypeMismatch) << src;
    }
}

// 下标运行期错误经异常通道可 catch,消息含越界值。
TEST(CodeGen, IndexErrorCaughtByTry) {
    auto out = run_source(R"(try { return [1][9]; } catch (e) { return str(e); })");
    ASSERT_TRUE(out.has_value()) << out.error().message();
    EXPECT_EQ(aria::format_value(*out), "Runtime: IndexOutOfBounds list index 9 out of range");
}

// 互环 == 判等(余归纳:同对重遇视为相等,两环展开同一棵无限树);EQUAL 弹栈后操作数
// 无根,equals 全程零分配方可在 stress GC 下存活。
TEST(CodeGen, CycleEqualsCoinductive) {
    auto out = run_source(R"(
var a = [1];
var b = [2];
a[0] = b;
b[0] = a;
return a == b;
)");
    EXPECT_EQ(aria::format_value(*out), "true");
}

// 写入新鲜对象跨 GC 点(run_source 开 stress GC,元素 list 经值栈/对象图级联保命)。
TEST(CodeGen, IndexWriteGcStress) {
    EXPECT_EQ(aria::format_value(*run_source(R"(
var xs = [[1], [2]];
xs[0] = ["aa", xs[1]];
return xs;
)")),
              "[[\"aa\", [2]], [2]]");
}
