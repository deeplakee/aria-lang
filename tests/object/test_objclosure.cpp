#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
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

    // 测试便利:intern + 守卫 name,再调 new_module(2-arg)。工厂不再替调用方守卫入参,故本助手显式
    // 守卫 name 跨 new_module 内部 new_string(cwd)/new_object。返回的 m 未根(守卫随函数退出释放),
    // 调用方跨 GC 点持有 m 须自行再守卫。默认名 "<script>"(机制测试不关心模块归属,临时模块)。
    ObjModule* make_module(GC& gc, StringView name = "<script>") {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        return aria::new_module(gc, nm);
    }

    // 指定模块的具名函数:intern + 守卫 name,守卫 m,调 aria::new_function。工厂不再守卫入参,
    // 故本助手显式守卫 m 与 name。m 须在 new_string(name) 之前入根(name 分配可能 collect 回收 m)。
    ObjFunction* make_function(GC& gc, ObjModule* m, StringView name, u8 arity) {
        auto guard = gc.make_guard(m);
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return aria::new_function(gc, m, nm, arity);
    }

    // 3 参便利重载:造临时模块 + 委托 4 参 aria::new_function。屏蔽全局 aria::new_function。
    // name=nullptr -> `<main>`(主入口单元统一名,ObjFunction ctor ASSERT name 非空)。
    ObjFunction* new_function(GC& gc, ObjString* name, u8 arity) {
        if (name == nullptr) {
            name = new_string(gc, "<main>");
        }
        auto guard = gc.make_guard(name);
        auto m     = make_module(gc);
        guard.push(m);
        return aria::new_function(gc, m, name, arity);
    }

} // namespace

// 基础:包住 fn,upvalues_ 空态(CLOSURE 执行期才逐个后填)。
TEST(ObjClosure, WrapsFunction) {
    GC   gc;
    auto fn = new_function(gc, new_string(gc, "add"), 2);
    auto c  = new_closure(gc, fn);
    EXPECT_TRUE(aria::Object::is<ObjClosure>(c));
    EXPECT_EQ(c->type(), aria::ObjType::CLOSURE);
    EXPECT_EQ(c->type_name(), "Closure");
    EXPECT_EQ(c->function(), fn);
    EXPECT_EQ(c->upvalue_count(), usize{0});
    EXPECT_TRUE(c->upvalues().empty());
}

// 捕获数组:逐个后填,按下标可取。
TEST(ObjClosure, AddUpvaluesInOrder) {
    GC    gc;
    auto  fn    = new_function(gc, new_string(gc, "f"), 0);
    auto  c     = new_closure(gc, fn);
    auto  guard = gc.make_guard(c); // 习惯性守卫(非 stress 下无 GC 触发)
    Value a     = Value::from_i32(1);
    Value b     = Value::from_i32(2);
    auto  u1    = new_upvalue(gc, &a);
    auto  u2    = new_upvalue(gc, &b);
    c->add_upvalue(u1);
    c->add_upvalue(u2);
    EXPECT_EQ(c->upvalue_count(), usize{2});
    EXPECT_EQ(c->upvalues()[0], u1);
    EXPECT_EQ(c->upvalues()[1], u2);
}

// to_string 委托 function_->to_string():渲染 `<fn name>`,与纯函数同文案。
TEST(ObjClosure, ToStringDelegatesToFunction) {
    GC   gc;
    auto fn = new_function(gc, new_string(gc, "add"), 0);
    auto c  = new_closure(gc, fn);
    EXPECT_EQ(c->to_string(), "<fn add>");
    // 调试渲染同文案(format_value_debug 的 CLOSURE 分支,非虚路径)。
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(c)), "<fn add>");
}

// 身份语义:同一 fn 的两次捕获是不同闭包;equals/=== 均按地址。
TEST(ObjClosure, IdentitySemantics) {
    GC   gc;
    auto fn = new_function(gc, new_string(gc, "f"), 0);
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
    auto fn    = new_function(gc, new_string(gc, "f"), 0);
    auto guard = gc.make_guard(fn);   // stress 下先根化 fn(c 建好后另经 c 可达)
    auto c     = new_closure(gc, fn); // 建时 collect:fn 经 guard 存活
    guard.push(c);
    auto constant = new_string(gc, "a long constant string beyond sso padding");
    fn->unit().add_constant(Value::from_obj(constant)); // push 走 trivial 分配不触 GC
    Value v  = Value::nil_val();
    auto  uv = new_upvalue(gc, &v);                                                 // 建时 collect:fn/c 经 guard 存活
    c->add_upvalue(uv);                                                             // 先入 c(根可达)再跨 GC
    v = Value::from_obj(new_string(gc, "captured long string beyond sso padding")); // 建时 collect:uv 经 c 存活
    (void) new_string(gc, "trigger"); // stress collect:全链经 c.trace 存活
    EXPECT_EQ(c->function(), fn);
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    ASSERT_EQ(c->upvalue_count(), usize{1});
    EXPECT_EQ(c->upvalues()[0], uv);
    EXPECT_TRUE(value_identical(*c->upvalues()[0]->value_slot(), v));
}

// 无根闭包被 sweep(闭包壳 + fn 壳 + CodeUnit 内部 Array)。
TEST(ObjClosure, UnrootedClosureSwept) {
    GC   gc;
    auto fn = new_function(gc, new_string(gc, "f"), 0);
    (void) new_closure(gc, fn); // 两者皆无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}
