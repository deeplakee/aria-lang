#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
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
using aria::ObjException;
using aria::ObjFunction;
using aria::ObjInstance;
using aria::ObjModule;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_obj;
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

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.main_context().take_error();
        EXPECT_TRUE(payload.has_value()); // fail 契约:失败信号 ⟺ 寄存器必有载荷
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
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
// trace 覆盖上(漏标即丢)。栽种走真实协议路径(字段经 store_field、缓存经 load_field 现场绑定)。
TEST(ObjInstance, TraceStressKeepsClassFieldsAndCachedBound) {
    AriaVM vm;
    auto&  gc = vm.gc();
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
        method->set_defining_class(cls); // 戳方法性(MAKE_METHOD 注册等价形;不戳则 load_field 直读不绑定)
        bkey = new_string(gc, "m");      // 建时 collect:在根者存活
        g.push(bkey);
        cls->set_field(bkey, Value::from_obj(method)); // 注册方法(建表/rehash 非 GC 点)
        auto bound_read = obj->load_field(vm, bkey);   // 绑定 + 回填 fields 缓存(真实缓存路径;stress 下
        ASSERT_TRUE(bound_read.has_value());           //   new_bound_method 分配时 obj/cls/method 皆在根,安全)
        bound = aria::Object::try_as<ObjBoundMethod>(bound_read->as_obj());
        ASSERT_NE(bound, nullptr);
        fkey = new_string(gc, "x");
        g.push(fkey);
        fval = new_string(gc, "a long field value string!!!"); // 建时 collect:在根者存活
        g.push(fval);
        EXPECT_TRUE(obj->store_field(vm, fkey, Value::from_obj(fval))); // 真字段写入
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
    auto bfound = obj->load_field(vm, bkey); // fields 命中:collect 后缓存 bound 原样直取
    ASSERT_TRUE(bfound.has_value());
    EXPECT_TRUE(value_identical(*bfound, Value::from_obj(bound)));
    auto ffound = obj->load_field(vm, fkey);
    ASSERT_TRUE(ffound.has_value());
    EXPECT_TRUE(value_identical(*ffound, Value::from_obj(fval)));
    EXPECT_TRUE(value_identical(bound->method(),
                                Value::from_obj(method))); // bound 经 fields 级联存活,method/闭包 fn 全链随活
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

// ---- 成员访问协议 override ----

// load_field 分流:fields 命中优先(铁则 3)→ 委托类协议(ObjClass::load_field 沿链读
// 穿透直读,类协议不绑定不缓存):方法闭包(看 defining class 戳不看值类型)绑 this 并回填
// fields 缓存(铁则 1,快照语义)、其余直读不缓存、全链 miss 随类措辞 fail(本 override 只透传)。
TEST(ObjInstance, LoadFieldBindsCachesAndReadsStatic) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto cls = make_class(gc, "Foo");
    guard.push(cls);
    auto inst = make_instance(gc, cls);
    guard.push(inst);

    // 类表静态值:直读、不缓存 --行为钉法:类上覆写后实例再读到新值(若被缓存则读陈旧)。
    auto vkey = new_string(gc, "sv");
    guard.push(vkey);
    auto sv = new_string(gc, "a static value string!!!!!!!!!!");
    guard.push(sv);
    cls->set_field(vkey, Value::from_obj(sv));
    auto static_read = inst->load_field(vm, vkey);
    ASSERT_TRUE(static_read.has_value());
    EXPECT_TRUE(value_identical(*static_read, Value::from_obj(sv)));
    auto sv2  = new_string(gc, "a static value string rewritten!!");
    auto sv2g = gc.make_guard(sv2);
    cls->set_field(vkey, Value::from_obj(sv2)); // 类上原槽更新(不缓存 ⟹ 实例再读见新值)
    auto rewritten_read = inst->load_field(vm, vkey);
    ASSERT_TRUE(rewritten_read.has_value());
    EXPECT_TRUE(value_identical(*rewritten_read, Value::from_obj(sv2))); // 无陈旧缓存(铁则 1)

    // 类表方法(闭包 + defining class 戳 = 方法性标记,对象层 MAKE_METHOD 注册等价形):
    // 绑定 ObjBoundMethod(receiver=inst)并回填 fields 缓存。
    auto mkey = new_string(gc, "m");
    guard.push(mkey);
    auto method = make_closure(gc, "m", 0);
    guard.push(method);
    method->set_defining_class(cls); // 戳定方法性(不戳则直读不绑,见下方 lambda 钉子)
    cls->set_field(mkey, Value::from_obj(method));
    auto method_read = inst->load_field(vm, mkey);
    ASSERT_TRUE(method_read.has_value());
    auto bound = aria::Object::try_as<ObjBoundMethod>(method_read->as_obj());
    ASSERT_NE(bound, nullptr);
    EXPECT_TRUE(value_identical(bound->receiver(), Value::from_obj(inst))); // this=本实例
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(method)));

    // 二次读同键:fields 命中优先,直取缓存项(不再新建绑定)。
    auto cached_read = inst->load_field(vm, mkey);
    ASSERT_TRUE(cached_read.has_value());
    EXPECT_TRUE(value_identical(*cached_read, *method_read)); // 同一缓存项(=== 指针相等)

    // 静态槽持未戳闭包(lambda,无 defining class 戳):原值直读不绑定(方法性看戳不看值类型)。
    auto hkey = new_string(gc, "h");
    guard.push(hkey);
    auto lam = make_closure(gc, "h", 0);
    guard.push(lam);
    cls->set_field(hkey, Value::from_obj(lam));
    auto lambda_read = inst->load_field(vm, hkey);
    ASSERT_TRUE(lambda_read.has_value());
    EXPECT_TRUE(value_identical(*lambda_read, Value::from_obj(lam))); // === 原闭包,无 ObjBoundMethod 包装

    // 真字段遮蔽同名方法与缓存项(铁则 3):this.m = 9 走 store_field 后读到字段值。
    EXPECT_TRUE(inst->store_field(vm, mkey, Value::from_int(9)));
    auto shadowed_read = inst->load_field(vm, mkey);
    ASSERT_TRUE(shadowed_read.has_value());
    EXPECT_EQ(shadowed_read->as_int(), 9);

    // 全链 miss:委托类协议,随类措辞 fail(UndefinedProperty,消息含宿主类 debug 渲染;
    // 实例不再自持措辞 --成员表在类链上,文案随宿主)。
    auto miss = new_string(gc, "missing");
    guard.push(miss);
    EXPECT_FALSE(inst->load_field(vm, miss).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("<class Foo> has no member 'missing'"));
}

// store_field:实例字段动态 upsert,永不失败(恒 true;false ⟺ 已 fail)。
TEST(ObjInstance, StoreFieldDynamicUpsert) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto cls = make_class(gc, "Foo");
    guard.push(cls);
    auto inst = make_instance(gc, cls);
    guard.push(inst);

    auto k = new_string(gc, "x");
    guard.push(k);
    EXPECT_TRUE(inst->store_field(vm, k, Value::from_int(1))); // 即创建
    auto created_read = inst->load_field(vm, k);
    ASSERT_TRUE(created_read.has_value());
    EXPECT_EQ(created_read->as_int(), 1);
    EXPECT_TRUE(inst->store_field(vm, k, Value::from_int(2))); // 原槽更新
    auto updated_read = inst->load_field(vm, k);
    ASSERT_TRUE(updated_read.has_value());
    EXPECT_EQ(updated_read->as_int(), 2);
}
