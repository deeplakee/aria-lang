#include <gtest/gtest.h>

#include <tuple>

#include "bytecode/code.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

using aria::GC;
using aria::new_module;
using aria::new_string;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {

    // 测试便利:委托 new_module 1 参 StringView 重载(name/dir 经工厂内部 intern 并自守)。
    // 返回的 m 未根,调用方跨 GC 点持有须自行守卫。默认名 "<script>"(临时模块)。
    ObjModule* make_module(GC& gc, const StringView name = "<script>") { return new_module(gc, name); }

    // 指定模块的具名函数:m 未根须自守(工厂内 intern name 与 new_object 均 GC 点,可能
    // collect 回收 m)。返回白色,调用方自守。无缺省、无 varargs。
    ObjFunction* make_function(GC& gc, ObjModule* m, const StringView name, const u8 arity) {
        auto guard = gc.make_guard(m);
        return aria::new_function(gc, m, name, arity, arity, false); // min_arity = arity
    }

    // 3 参便利重载:造临时模块 + 委托工厂 StringView 重载。屏蔽全局 aria::new_function。
    // m 未根须自守:make_module 与工厂内部分配在 stress GC 下会 collect,裸局部指针无根会被
    // 扫掉;name 串由工厂内部 intern 并自守。
    ObjFunction* new_function(GC& gc, const StringView name, const u8 arity) {
        auto m     = make_module(gc);
        auto guard = gc.make_guard(m);
        return aria::new_function(gc, m, name, arity, arity, false); // min_arity = arity
    }

} // namespace

// varargs 标志:工厂尾参入对象;arity 语义为**固定参数数**(rest 不计,帧槽深 = arity + 1)。
TEST(ObjFunction, VarargsFlag) {
    GC   gc;
    auto m      = make_module(gc);
    auto guard  = gc.make_guard(m);
    auto var_fn = aria::new_function(gc, m, "f", 2, 1, true);
    guard.push(var_fn);
    EXPECT_TRUE(var_fn->is_varargs());
    EXPECT_EQ(var_fn->arity(), 2); // rest 不计
    EXPECT_EQ(var_fn->min_arity(), 1);

    auto plain = aria::new_function(gc, m, "g", 3, 3, false);
    guard.push(plain);
    EXPECT_FALSE(plain->is_varargs());
    EXPECT_EQ(plain->arity(), 3);
}

TEST(ObjFunction, Basics) {
    GC   gc;
    auto name = new_string(gc, "add");
    auto fn   = new_function(gc, name->view(), 2);
    EXPECT_TRUE(fn->is<ObjFunction>());
    EXPECT_EQ(fn->type(), aria::ObjType::FUNCTION);
    EXPECT_EQ(fn->name(), name); // 同名 intern 同指针
    EXPECT_EQ(fn->arity(), 2);
    EXPECT_TRUE(fn->unit().code.empty()); // 初始空 CodeUnit
    EXPECT_TRUE(fn->unit().constants.empty());
    EXPECT_TRUE(fn->unit().lines.empty());
}

TEST(ObjFunction, EmitIntoUnit) {
    GC    gc;
    auto  fn = new_function(gc, "mul", 1);
    auto& cu = fn->unit();
    cu.emit_op(aria::OpCode::LOAD_CONST, 3);
    cu.emit_word(cu.add_constant(Value::from_i32(42)), 3);
    cu.emit_op(aria::OpCode::RETURN, 4);
    EXPECT_EQ(cu.code.size(), usize{4}); // 1 opcode + 2 操作数 + 1 opcode
    EXPECT_EQ(cu.line_for_offset(0), 3);
    EXPECT_EQ(cu.line_for_offset(3), 4);
    EXPECT_EQ(cu.constants.size(), usize{1});
}

TEST(ObjFunction, ToString) {
    GC   gc;
    auto fn = new_function(gc, "add", 0);
    EXPECT_EQ(fn->to_string(), "<fn add>");

    auto script = new_function(gc, "<main>", 0); // 匿名入口单元(<main> 名)
    EXPECT_EQ(script->to_string(), "<fn <main>>");
}

TEST(ObjFunction, TraceMarksNameAndConstants) {
    GC gc;
    gc.set_stress(true);
    auto fn    = new_function(gc, "add", 2);
    auto guard = gc.make_guard(fn); // stress 下后续任何 new_object 都会 collect,先保住 fn
    // 长串常量(独立 buffer,被回收则内容不可访问)
    auto constant = new_string(gc, "a long constant string beyond sso");
    fn->unit().add_constant(Value::from_obj(constant));
    std::ignore = new_string(gc, "trigger"); // stress 触发 collect:name/常量须经 fn 的 trace 存活
    EXPECT_EQ(fn->name()->view(), "add");
    EXPECT_EQ(constant->view(), "a long constant string beyond sso");
}

TEST(ObjFunction, UnrootedFunctionSwept) {
    GC gc;
    std::ignore        = new_function(gc, "f", 0); // 无根
    const usize before = gc.bytes_allocated();
    gc.collect(); // 壳 + CodeUnit 内部 Array(此处未 emit,容量 0)回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

TEST(ObjFunction, SweptAfterGuardReleased) {
    GC gc;
    gc.set_stress(true);
    auto fn = new_function(gc, "add", 0);
    {
        auto guard  = gc.make_guard(fn);
        std::ignore = new_string(gc, "trigger"); // GC:fn 存活
        EXPECT_EQ(fn->to_string(), "<fn add>");
    } // guard 析构 pop 临时根 -> fn 不再受保护
    const usize before = gc.bytes_allocated();
    std::ignore        = new_string(gc, "trigger2"); // GC:fn 与其 name 串(无引用)回收
    EXPECT_LT(gc.bytes_allocated(), before);
}

// module_ 回指:构造时传入模块,不可变、非空。
TEST(ObjFunction, ModuleBackref) {
    GC   gc;
    auto m  = make_module(gc, "lib/utils");
    auto fn = make_function(gc, m, "f", 0);
    EXPECT_EQ(fn->module(), m); // 构造时确定
}

// trace 标 module_:fn 入根 -> fn.trace 标 module_ -> 模块(及模块 name_)存活。
// 验证 module <-> entry 环不影响 mark-sweep(module_ 不被根,仅经 fn.trace 可达)。
TEST(ObjFunction, TraceMarksModule) {
    GC   gc;
    auto m  = make_module(gc, "lib/utils"); // 模块不单独根
    auto fn = make_function(gc, m, "f", 0); // m 经 make_function 内部 guard 存活至 fn 入根
    // 仅根 fn:m 须经 fn.trace(module_) 存活
    auto        guard  = gc.make_guard(fn);
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before); // m + m->name_ 经 fn.trace 存活
    EXPECT_EQ(fn->module(), m);
    EXPECT_EQ(m->name()->view(), "lib/utils");
}
