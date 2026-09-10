#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_bound_method;
using aria::new_class;
using aria::new_closure;
using aria::new_function;
using aria::new_instance;
using aria::new_module;
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

    // 实例:守卫 cls 跨 new_instance。返回的 inst 未根,调用方跨 GC 点持有须自行守卫。
    ObjInstance* make_instance(GC& gc, ObjClass* cls) {
        auto guard = gc.make_guard(cls);
        return new_instance(gc, cls);
    }

} // namespace

TEST(ObjInstance, Basics) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto cg  = gc.make_guard(cls);
    auto obj = new_instance(gc, cls);
    EXPECT_TRUE(aria::Object::is<ObjInstance>(obj));
    EXPECT_EQ(obj->type(), aria::ObjType::INSTANCE);
    EXPECT_EQ(obj->cls(), cls);
    EXPECT_EQ(obj->fields().size(), 0u); // 惰性:shell 建成即无表,字段全动态
}

TEST(ObjInstance, FieldsUpsertAndFind) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto cg  = gc.make_guard(cls);
    auto obj = make_instance(gc, cls);
    cg.push(obj);

    auto k = new_string(gc, "x");
    cg.push(k);
    auto v = new_string(gc, "a long field value string!!!");
    cg.push(v);
    auto* e = obj->fields().upsert(Value::from_obj(k));
    ASSERT_NE(e, nullptr);
    e->value = Value::from_obj(v);

    auto* found = obj->fields().find(Value::from_obj(k));
    ASSERT_NE(found, nullptr);
    EXPECT_TRUE(value_identical(found->value, Value::from_obj(v)));
    EXPECT_EQ(obj->fields().size(), 1u);
}

TEST(ObjInstance, ToString) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto cg  = gc.make_guard(cls);
    auto obj = new_instance(gc, cls);
    EXPECT_EQ(obj->to_string(), "<Foo instance>");
}

TEST(ObjInstance, DebugRender) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto cg  = gc.make_guard(cls);
    auto obj = new_instance(gc, cls);
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(obj)), "<Foo instance>"); // 调试渲染同文案
}

// stress GC:实例为唯一根,其类(经 class_)、字段长串、缓存 bound(经 fields 值级联,再经
// bound.trace 级联其 method 闭包与 receiver 即本实例)全部存活。守卫全部作用域弹出,断言压在
// trace 覆盖上(漏标即丢)。
TEST(ObjInstance, TraceStressKeepsClassFieldsAndCachedBound) {
    GC gc;
    gc.set_stress(true);

    ObjClass*       cls    = nullptr;
    ObjInstance*    obj    = nullptr;
    ObjClosure*     method = nullptr;
    ObjBoundMethod* bound  = nullptr;
    ObjString*      bkey   = nullptr;
    ObjString*      fkey   = nullptr;
    ObjString*      fval   = nullptr;
    {
        auto g = gc.make_guard();
        cls    = make_class(gc, "Foo"); // 建时 collect:name 经助手内守卫
        g.push(cls);
        obj = make_instance(gc, cls); // 建时 collect:cls 经守卫存活
        g.push(obj);
        method = make_closure(gc, "m", 0); // 建时 collect:cls/obj 经守卫存活
        g.push(method);
        bound = new_bound_method(gc, Value::from_obj(method), Value::from_obj(obj)); // 建时 collect:method/obj 经守卫存活
        g.push(bound);
        bkey = new_string(gc, "m"); // 建时 collect:在根者存活
        g.push(bkey);
        auto* bslot  = obj->fields().upsert(Value::from_obj(bkey)); // 建表/rehash 非 GC 点(trivial 分配)
        bslot->value = Value::from_obj(bound);                      // bound-method 缓存写入 fields
        fkey         = new_string(gc, "x");
        g.push(fkey);
        fval = new_string(gc, "a long field value string!!!"); // 建时 collect:在根者存活
        g.push(fval);
        auto* fslot  = obj->fields().upsert(Value::from_obj(fkey));
        fslot->value = Value::from_obj(fval);
        // 作用域退出:全部临时根弹出,cls/method/bound/fval 此后仅经 obj.trace 可达
    }
    auto        guard   = gc.make_guard(obj);        // 只根实例
    const auto  trigger = new_string(gc, "trigger"); // stress collect:全链经 obj.trace 存活
    auto        tg      = gc.make_guard(trigger);    // 触发串入根:不被下方 collect 回收
    const usize before  = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(obj->cls(), cls);
    EXPECT_EQ(cls->name()->view(), "Foo"); // 类经实例存活,其 name_ 级联存活
    EXPECT_EQ(fval->view(), "a long field value string!!!");
    EXPECT_TRUE(value_identical(obj->fields().find(Value::from_obj(bkey))->value, Value::from_obj(bound)));
    EXPECT_TRUE(value_identical(obj->fields().find(Value::from_obj(fkey))->value, Value::from_obj(fval)));
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(method))); // bound 经 fields 级联存活,method/闭包 fn 全链随活
}

// 未根实例被 sweep(壳 + 其 class_/fields 值若无他根一并回收)。
TEST(ObjInstance, UnrootedInstanceSwept) {
    GC   gc;
    auto cls = make_class(gc, "orphan");
    (void) make_instance(gc, cls); // 双双无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}
