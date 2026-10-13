#include <gtest/gtest.h>

#include <tuple>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_closure;
using aria::new_string;
using aria::new_upvalue;
using aria::ObjClosure;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::ObjUpvalue;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;
using aria::value_equal;
using aria::value_identical;

namespace {

    // 测试便利:委托 new_module 1 参 StringView 重载(name/dir 经工厂内部 intern 并自守)。
    // 返回的 m 未根,调用方跨 GC 点持有须自行守卫。默认名 "<script>"(临时模块)。
    ObjModule* make_module(GC& gc, const StringView name = "<script>") { return aria::new_module(gc, name); }

    // 指定模块的具名函数:m 未根须自守(工厂内 intern name 与 new_object 均 GC 点,可能
    // collect 回收 m)。返回白色,调用方自守。
    ObjFunction* make_function(GC& gc, ObjModule* m, const StringView name, const u8 arity) {
        auto guard = gc.make_guard(m);
        return aria::new_function(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 3 参便利重载:造临时模块 + 委托工厂 StringView 重载。屏蔽全局 aria::new_function。
    // m 未根须自守:make_module 与工厂内部分配在 stress GC 下会 collect,裸局部指针无根会被
    // 扫掉;name 串由工厂内部 intern 并自守。
    ObjFunction* new_function(GC& gc, const StringView name, const u8 arity) {
        auto m     = make_module(gc);
        auto guard = gc.make_guard(m);
        return aria::new_function(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt ⟺ 寄存器必有载荷)。
    aria::Pair<aria::ErrorCode, aria::String> take_pending_error(aria::AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = aria::try_as_obj<aria::ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), aria::String{ex->message()->view()}};
    }

} // namespace

// 基础:包住 fn,upvalues_ 空态(CLOSURE 执行期才逐个后填)。
TEST(ObjClosure, WrapsFunction) {
    GC   gc;
    auto fn = new_function(gc, "add", 2);
    auto c  = new_closure(gc, fn);
    EXPECT_TRUE(c->is<ObjClosure>());
    EXPECT_EQ(c->type(), aria::ObjType::CLOSURE);
    EXPECT_EQ(c->type_name(), "Closure");
    EXPECT_EQ(c->function(), fn);
    EXPECT_TRUE(c->upvalues().empty());
}

// 捕获数组:逐个后填,按下标可取。
TEST(ObjClosure, AddUpvaluesInOrder) {
    GC    gc;
    auto  fn    = new_function(gc, "f", 0);
    auto  c     = new_closure(gc, fn);
    auto  guard = gc.make_guard(c); // 习惯性守卫(非 stress 下无 GC 触发)
    Value a     = Value::from_i32(1);
    Value b     = Value::from_i32(2);
    auto  u1    = new_upvalue(gc, &a);
    auto  u2    = new_upvalue(gc, &b);
    c->add_upvalue(u1);
    c->add_upvalue(u2);
    EXPECT_EQ(c->upvalues().size(), usize{2});
    EXPECT_EQ(c->upvalues()[0], u1);
    EXPECT_EQ(c->upvalues()[1], u2);
}

// debug_repr 直取 function_ 名渲染 `<fn name>`,与纯函数同文案。
TEST(ObjClosure, DebugReprSameAsFunction) {
    GC   gc;
    auto fn = new_function(gc, "add", 0);
    auto c  = new_closure(gc, fn);
    EXPECT_EQ(c->debug_repr(), "<fn add>");
    // 调试渲染同文案(repr 经 debug_repr 虚分派)。
    EXPECT_EQ(aria::repr(Value::from_obj(c)), "<fn add>");
}

// 身份语义:同一 fn 的两次捕获是不同闭包;equals/=== 均按地址。
TEST(ObjClosure, IdentitySemantics) {
    GC   gc;
    auto fn = new_function(gc, "f", 0);
    auto c1 = new_closure(gc, fn);
    auto c2 = new_closure(gc, fn);
    EXPECT_NE(c1, c2);
    EXPECT_TRUE(value_identical(Value::from_obj(c1), Value::from_obj(c1)));
    EXPECT_FALSE(value_identical(Value::from_obj(c1), Value::from_obj(c2)));
    EXPECT_FALSE(value_equal(Value::from_obj(c1), Value::from_obj(c2))); // equals 默认地址相等
}

// stress GC:仅根闭包,fn(含常量池长串)与 upvalue(含栈槽内捕获串)全链经 c.trace 存活。
TEST(ObjClosure, TraceMarksFunctionAndUpvalues) {
    GC gc;
    gc.set_stress(true);
    auto fn    = new_function(gc, "f", 0);
    auto guard = gc.make_guard(fn);   // stress 下先根化 fn(c 建好后另经 c 可达)
    auto c     = new_closure(gc, fn); // 建时 collect:fn 经 guard 存活
    guard.push(c);
    auto constant = new_string(gc, "a long constant string beyond sso padding");
    fn->unit().add_constant(Value::from_obj(constant)); // push 走 trivial 分配不触 GC
    Value v = Value::nil_val();
    // 建时 collect:fn/c 经 guard 存活
    auto uv = new_upvalue(gc, &v);
    c->add_upvalue(uv); // 先入 c(根可达)再跨 GC
    // 建时 collect:uv 经 c 存活
    v           = Value::from_obj(new_string(gc, "captured long string beyond sso padding"));
    std::ignore = new_string(gc, "trigger"); // stress collect:全链经 c.trace 存活
    EXPECT_EQ(c->function(), fn);
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    ASSERT_EQ(c->upvalues().size(), usize{1});
    EXPECT_EQ(c->upvalues()[0], uv);
    EXPECT_TRUE(value_identical(*c->upvalues()[0]->value_slot(), v));
}

// 无根闭包被 sweep(闭包壳 + fn 壳 + CodeUnit 内部 Array)。
TEST(ObjClosure, UnrootedClosureSwept) {
    GC   gc;
    auto fn            = new_function(gc, "f", 0);
    std::ignore        = new_closure(gc, fn); // 两者皆无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// 类面接入:闭包成员读经 Closure 链命中根(is_a/__str__ 原生可及),miss 以类措辞报。
TEST(ObjClosure, ChainMembersResolvable) {
    aria::AriaVM vm;
    auto&        gc    = vm.gc();
    auto         guard = gc.make_guard();
    auto         fn    = new_function(gc, "worker", 0);
    guard.push(fn);
    auto closure = new_closure(gc, fn);
    guard.push(closure);

    const auto is_a_key = new_string(gc, "is_a");
    guard.push(is_a_key);
    const auto hit = closure->load_field(*vm.current_context(), is_a_key);
    ASSERT_TRUE(hit.has_value());
    EXPECT_TRUE(hit->as_obj()->is<aria::ObjNativeFn>()); // 沿 Closure 链继承根原生

    const auto nope = new_string(gc, "nope");
    guard.push(nope);
    EXPECT_FALSE(closure->load_field(*vm.current_context(), nope).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, aria::ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class Closure> has no member 'nope'");
}
