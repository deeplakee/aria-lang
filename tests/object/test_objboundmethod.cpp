#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_bound_method;
using aria::new_class;
using aria::new_closure;
using aria::new_function;
using aria::new_instance;
using aria::new_module;
using aria::new_native_fn;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjClass;
using aria::ObjClosure;
using aria::ObjFunction;
using aria::ObjInstance;
using aria::ObjModule;
using aria::ObjString;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;
using aria::value_equal;
using aria::value_identical;

namespace {

    // intern + 守卫 name(super 非空时一并守卫),再调 new_class。返回的 cls 未根。
    ObjClass* make_class(GC& gc, StringView name, ObjClass* super = nullptr) {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        if (super != nullptr) {
            guard.push(super);
        }
        return new_class(gc, nm, super);
    }

    // 临时模块 + 具名闭包:intern + 守卫均收口在助手内。返回的闭包未根,fn 经其可达。
    ObjClosure* make_closure(GC& gc, StringView name, u8 arity) {
        auto m     = new_module(gc, StringView{"<script>"});
        auto guard = gc.make_guard(m);
        auto nm    = new_string(gc, name);
        guard.push(nm);
        auto fn = new_function(gc, m, nm, arity);
        guard.push(fn);
        return new_closure(gc, fn);
    }

    // 实例:守卫 cls 跨 new_instance。返回的 inst 未根。
    ObjInstance* make_instance(GC& gc, ObjClass* cls) {
        auto guard = gc.make_guard(cls);
        return new_instance(gc, cls);
    }

    // 原生方法绑定测试用的空实现(NativeFn 契约形;绑定测试不实际调用)。
    bool noop_native(aria::AriaVM& /*vm*/, aria::Span<Value> /*slots*/) { return true; }

} // namespace

TEST(ObjBoundMethod, Basics) {
    GC   gc;
    auto method = make_closure(gc, "m", 1);
    auto mg     = gc.make_guard(method);
    auto cls    = make_class(gc, "Foo");
    auto cg     = gc.make_guard(cls);
    auto inst   = make_instance(gc, cls);
    mg.push(inst);

    auto bound = new_bound_method(gc, Value::from_obj(method), Value::from_obj(inst)); // 建时(非 stress)无 collect
    EXPECT_TRUE(aria::Object::is<ObjBoundMethod>(bound));
    EXPECT_EQ(bound->type(), aria::ObjType::BOUND_METHOD);
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(method)));
    EXPECT_TRUE(value_identical(bound->receiver(), Value::from_obj(inst)));
}

// 绑定按身份判等:同一方法绑不同实例是不同对象(equals 默认地址相等)。
TEST(ObjBoundMethod, IdentitySemantics) {
    GC   gc;
    auto method = make_closure(gc, "m", 1);
    auto mg     = gc.make_guard(method);
    auto cls    = make_class(gc, "Foo");
    auto cg     = gc.make_guard(cls);
    auto i1     = make_instance(gc, cls);
    mg.push(i1);
    auto i2 = make_instance(gc, cls);
    mg.push(i2);

    auto b1 = new_bound_method(gc, Value::from_obj(method), Value::from_obj(i1));
    auto b2 = new_bound_method(gc, Value::from_obj(method), Value::from_obj(i2));
    EXPECT_NE(b1, b2);
    EXPECT_TRUE(value_identical(Value::from_obj(b1), Value::from_obj(b1)));
    EXPECT_FALSE(value_identical(Value::from_obj(b1), Value::from_obj(b2)));
    EXPECT_FALSE(value_equal(Value::from_obj(b1), Value::from_obj(b2))); // equals 默认地址相等
}

TEST(ObjBoundMethod, ToString) {
    GC   gc;
    auto method = make_closure(gc, "m", 1);
    auto mg     = gc.make_guard(method);
    auto cls    = make_class(gc, "Foo");
    auto cg     = gc.make_guard(cls);
    auto inst   = make_instance(gc, cls);
    mg.push(inst);
    auto bound = new_bound_method(gc, Value::from_obj(method), Value::from_obj(inst));
    EXPECT_EQ(bound->to_string(), "<bound method m>");
}

// 原生方法绑定(M5 泛化):method_ 可为 ObjNativeFn -- 内建类型方法的载体;
// name() 非虚取名分派(闭包 fn 名 / 原生 name_),to_string 与调试渲染同文案。
TEST(ObjBoundMethod, NativeMethodBinding) {
    GC   gc;
    auto native = new_native_fn(gc, "echo", noop_native); // StringView 重载:名字经工厂内部 intern 并自守
    auto guard  = gc.make_guard(native); // native 是 weak root,跨下方 make_class/make_instance 分配先保
    auto cls    = make_class(gc, "Foo");
    auto cg     = gc.make_guard(cls);
    auto inst   = make_instance(gc, cls);
    guard.push(inst);

    auto bound = new_bound_method(gc, Value::from_obj(native), Value::from_obj(inst));
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(native)));
    EXPECT_EQ(bound->name()->view(), "echo");
    EXPECT_EQ(bound->to_string(), "<bound method echo>");
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(bound)), "<bound method echo>"); // 调试渲染同文案
}

TEST(ObjBoundMethod, DebugRender) {
    GC   gc;
    auto method = make_closure(gc, "m", 1);
    auto mg     = gc.make_guard(method);
    auto cls    = make_class(gc, "Foo");
    auto cg     = gc.make_guard(cls);
    auto inst   = make_instance(gc, cls);
    mg.push(inst);
    auto bound = new_bound_method(gc, Value::from_obj(method), Value::from_obj(inst));
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(bound)), "<bound method m>"); // 调试渲染同文案
}

// stress GC:bound 为唯一根,method 闭包(经 method_,fn 持常量池长串)与 receiver 实例
// (经 receiver_ 值 mark_value,实例再级联其类)全部存活。守卫全部作用域弹出,断言压在
// trace 覆盖上(漏标即丢)。
TEST(ObjBoundMethod, TraceStressKeepsMethodAndReceiver) {
    GC gc;
    gc.set_stress(true);

    ObjClass*       cls      = nullptr;
    ObjInstance*    inst     = nullptr;
    ObjClosure*     method   = nullptr;
    ObjString*      constant = nullptr;
    ObjBoundMethod* bound    = nullptr;
    {
        auto g = gc.make_guard();
        cls    = make_class(gc, "Foo"); // 建时 collect:name 经助手内守卫
        g.push(cls);
        inst = make_instance(gc, cls); // 建时 collect:cls 经守卫存活
        g.push(inst);
        method = make_closure(gc, "m", 1); // 建时 collect:cls/inst 经守卫存活
        g.push(method);
        constant = new_string(gc, "a long constant string beyond sso padding"); // 建时 collect:在根者存活
        g.push(constant);
        // push 走 trivial 分配不触 GC
        method->function()->unit().add_constant(Value::from_obj(constant));
        // 建时 collect:经守卫存活
        bound = new_bound_method(gc, Value::from_obj(method), Value::from_obj(inst));
        g.push(bound);
        // 作用域退出:全部临时根弹出,method/inst/cls 此后仅经 bound.trace 可达
    }
    auto        guard   = gc.make_guard(bound);      // 只根 bound
    const auto  trigger = new_string(gc, "trigger"); // stress collect:全链经 bound.trace 存活
    auto        tg      = gc.make_guard(trigger);    // 触发串入根:不被下方 collect 回收
    const usize before  = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(method)));
    EXPECT_TRUE(value_identical(bound->receiver(), Value::from_obj(inst)));
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    EXPECT_EQ(inst->cls(), cls);
    EXPECT_EQ(cls->name()->view(), "Foo");
}

// 未根绑定方法被 sweep。
TEST(ObjBoundMethod, UnrootedBoundMethodSwept) {
    GC   gc;
    auto method = make_closure(gc, "m", 1);
    auto cls    = make_class(gc, "orphan");
    auto inst   = make_instance(gc, cls);
    (void) new_bound_method(gc, Value::from_obj(method), Value::from_obj(inst)); // 全链无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}
