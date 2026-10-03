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

    // name 经工厂 StringView 重载 intern 并自守;super 守卫承重(调用方传上一轮
    // make_class 返回的未根指针;可空,空则不守 -- 对齐生产形态,null 不进临时根)。
    // 返回的 klass 未根。
    ObjClass* make_class(GC& gc, const StringView name, ObjClass* super = nullptr) {
        auto guard = gc.make_guard();
        if (super != nullptr) {
            guard.push(super);
        }
        return new_class(gc, name, super);
    }

    // 临时模块 + 具名闭包:守卫收口在助手内。返回的闭包未根,fn 经其可达。m 未根须自守:
    // 工厂内 intern name 与 new_object 均 GC 点。
    ObjClosure* make_closure(GC& gc, const StringView name, const u8 arity) {
        auto m     = new_module(gc, StringView{"<script>"});
        auto guard = gc.make_guard(m);
        auto fn    = new_function(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
        guard.push(fn);
        return new_closure(gc, fn);
    }

    // 实例:守卫 klass 跨 new_instance。返回的 inst 未根,调用方跨 GC 点持有须自行守卫。
    ObjInstance* make_instance(GC& gc, ObjClass* klass) {
        auto guard = gc.make_guard(klass);
        return new_instance(gc, klass);
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value()); // fail 契约:失败信号 ⟺ 寄存器必有载荷
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

TEST(ObjInstance, Basics) {
    GC   gc;
    auto klass = make_class(gc, "Foo");
    auto guard = gc.make_guard(klass);
    auto obj   = new_instance(gc, klass);
    EXPECT_TRUE(aria::Object::is<ObjInstance>(obj));
    EXPECT_EQ(obj->type(), aria::ObjType::INSTANCE);
    EXPECT_EQ(obj->klass(), klass);
}

TEST(ObjInstance, ToString) {
    GC   gc;
    auto klass = make_class(gc, "Foo");
    auto guard = gc.make_guard(klass);
    auto obj   = new_instance(gc, klass);
    EXPECT_EQ(obj->to_string(), "<Foo instance>");
}

TEST(ObjInstance, DebugRender) {
    GC   gc;
    auto klass = make_class(gc, "Foo");
    auto guard = gc.make_guard(klass);
    auto obj   = new_instance(gc, klass);
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(obj)), "<Foo instance>"); // 调试渲染同文案
}

// stress GC:实例为唯一根,其类(经 class_)、字段长串、方法闭包(经「obj -> class_ -> 类表」链)
// 经 obj.trace 全链存活。守卫全部作用域弹出,断言压在 trace 覆盖上(漏标即丢)。栽种走真实协议
// 路径(字段经 store_field、方法闭包经类表注册)。
TEST(ObjInstance, TraceStressKeepsClassFieldsAndFreshBound) {
    AriaVM vm;
    auto&  gc = vm.gc();
    gc.set_stress(true);

    ObjClass*       klass  = nullptr;
    ObjInstance*    obj    = nullptr;
    ObjClosure*     method = nullptr;
    ObjBoundMethod* bound  = nullptr;
    ObjString*      bkey   = nullptr;
    ObjString*      fkey   = nullptr;
    ObjString*      fval   = nullptr;
    {
        auto guard = gc.make_guard();
        klass      = make_class(gc, "Foo"); // 建时 collect:name 经助手内守卫
        guard.push(klass);
        obj = make_instance(gc, klass); // 建时 collect:klass 经守卫存活
        guard.push(obj);
        method = make_closure(gc, "m", 0); // 建时 collect:klass/obj 经守卫存活
        guard.push(method);
        method->set_defining_class(klass); // 戳方法性(MAKE_METHOD 注册等价形;不戳则 load_field_bound 直读不绑定)
        bkey = new_string(gc, "m");        // 建时 collect:在根者存活
        guard.push(bkey);
        klass->set_field(bkey, Value::from_obj(method));   // 注册方法(建表/rehash 非 GC 点)
        auto bound_read = obj->load_field_bound(vm, bkey); // 现场绑定(读路径每次物化新 bound;stress 下
        //   new_bound_method 分配时 obj/klass/method 皆在根,安全)
        ASSERT_TRUE(bound_read.has_value());
        {
            // 首个 bound 只活在 C++ 局部,stress 下每次分配即回收 -> 先临时根化再二读,
            // 否则二读可能复用同一地址,身份比较失真。
            auto bound_guard = gc.make_guard(bound_read->as_obj());
            auto bound_again = obj->load_field_bound(vm, bkey); // 二次读:现场新绑对象,内容相等
            ASSERT_TRUE(bound_again.has_value());
            EXPECT_FALSE(value_identical(*bound_read, *bound_again));
            EXPECT_TRUE(aria::value_equal(*bound_read, *bound_again));
        }
        bound = aria::Object::try_as<ObjBoundMethod>(bound_read->as_obj());
        ASSERT_NE(bound, nullptr);
        fkey = new_string(gc, "x");
        guard.push(fkey);
        fval = new_string(gc, "a long field value string!!!"); // 建时 collect:在根者存活
        guard.push(fval);
        EXPECT_TRUE(obj->store_field(vm, fkey, Value::from_obj(fval))); // 真字段写入
        // 作用域退出:全部临时根弹出 -- klass/静态表(fval 之外的方法闭包)/fval 此后仅经
        // obj.trace 可达(bound 本体是函数局部临时值,不入任何表)
    }
    auto       guard   = gc.make_guard(obj);        // 只根实例
    const auto trigger = new_string(gc, "trigger"); // stress collect:全链经 obj.trace 存活
    guard.push(trigger);                            // 触发串入根:不被下方 collect 回收
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(obj->klass(), klass);
    EXPECT_EQ(klass->name()->view(), "Foo"); // 类经实例存活,其 name_ 级联存活
    EXPECT_EQ(fval->view(), "a long field value string!!!");
    auto ffound = obj->load_field_bound(vm, fkey); // 真字段经 fields 存活
    ASSERT_TRUE(ffound.has_value());
    EXPECT_TRUE(value_identical(*ffound, Value::from_obj(fval)));
    // 方法闭包经「obj -> class_ -> 类表」链存活:collect 后仍可现场重绑
    EXPECT_EQ(method->name()->view(), "m");
    auto after_collect = obj->load_field_bound(vm, bkey);
    ASSERT_TRUE(after_collect.has_value());
    const auto* rebound = aria::Object::try_as<ObjBoundMethod>(after_collect->as_obj());
    ASSERT_NE(rebound, nullptr);
    EXPECT_TRUE(value_identical(rebound->receiver(), Value::from_obj(obj)));
    EXPECT_TRUE(value_identical(rebound->method(), Value::from_obj(method))); // 实现在类表,全链随活
}

// 未根实例被 sweep(壳 + 其 class_/fields 值若无他根一并回收)。
TEST(ObjInstance, UnrootedInstanceSwept) {
    GC   gc;
    auto klass = make_class(gc, "orphan");
    make_instance(gc, klass); // 双双无根
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// ---- 成员访问协议 override ----

// load_field_bound 分流:fields 命中优先 → 委托类协议(ObjClass::load_field 沿链读穿透直读,类协议
// 不绑定不缓存):方法闭包(看 defining class 戳不看值类型)现场绑 this(**每次访问一个新 bound,
// 不写回 fields**)、其余直读、全链 miss 随类措辞 fail(本 override 只透传)。类/父类改写后实例
// 立即见新值(读与调用同一份可见性)。load_field(调用路径)与内置同规则:不绑定,给原值。
TEST(ObjInstance, LoadFieldBindsReadsStaticAndResolvesFresh) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto klass = make_class(gc, "Foo");
    guard.push(klass);
    auto inst = make_instance(gc, klass);
    guard.push(inst);

    // 类表静态值直读 --行为钉法:类上覆写后实例再读到新值(读路径不落实例侧副本)。
    auto vkey = new_string(gc, "sv");
    guard.push(vkey);
    auto sv = new_string(gc, "a static value string!!!!!!!!!!");
    guard.push(sv);
    klass->set_field(vkey, Value::from_obj(sv));
    auto static_read = inst->load_field_bound(vm, vkey);
    ASSERT_TRUE(static_read.has_value());
    EXPECT_TRUE(value_identical(*static_read, Value::from_obj(sv)));
    auto sv2 = new_string(gc, "a static value string rewritten!!");
    guard.push(sv2);
    klass->set_field(vkey, Value::from_obj(sv2)); // 类上原槽更新 ⟹ 实例再读见新值
    auto rewritten_read = inst->load_field_bound(vm, vkey);
    ASSERT_TRUE(rewritten_read.has_value());
    EXPECT_TRUE(value_identical(*rewritten_read, Value::from_obj(sv2))); // 类上改写立即可见

    // 类表方法(闭包 + defining class 戳 = 方法性标记,对象层 MAKE_METHOD 注册等价形):
    // 绑定 ObjBoundMethod(receiver=inst)。
    auto mkey = new_string(gc, "m");
    guard.push(mkey);
    auto method = make_closure(gc, "m", 0);
    guard.push(method);
    method->set_defining_class(klass); // 戳定方法性(不戳则直读不绑,见下方 lambda 钉子)
    klass->set_field(mkey, Value::from_obj(method));
    auto method_read = inst->load_field_bound(vm, mkey);
    ASSERT_TRUE(method_read.has_value());
    auto bound = aria::Object::try_as<ObjBoundMethod>(method_read->as_obj());
    ASSERT_NE(bound, nullptr);
    EXPECT_TRUE(value_identical(bound->receiver(), Value::from_obj(inst))); // this=本实例
    EXPECT_TRUE(value_identical(bound->method(), Value::from_obj(method)));

    // 二次读同键:现场重绑 -- 新对象(=== 假)但内容相等(== 真);分配计数两读各 +1
    // 即「每次访问物化新 bound」的判据。
    const auto allocs_before = gc.allocation_count();
    auto       first_read    = inst->load_field_bound(vm, mkey);
    auto       second_read   = inst->load_field_bound(vm, mkey);
    ASSERT_TRUE(first_read.has_value() && second_read.has_value());
    EXPECT_EQ(gc.allocation_count() - allocs_before, 2u);
    EXPECT_FALSE(value_identical(*first_read, *second_read));
    EXPECT_TRUE(aria::value_equal(*first_read, *second_read));

    // 调用路径解析(load_field)不绑定:直接给类表里的方法闭包原值(槽 0 由 VM 交 receiver)。
    auto invoke_target = inst->load_field(vm, mkey);
    ASSERT_TRUE(invoke_target.has_value());
    EXPECT_TRUE(value_identical(*invoke_target, Value::from_obj(method)));
    // 真字段优先:字段里存的任意值原值直调(可调用与否由 VM 侧判定)。
    EXPECT_TRUE(inst->store_field(vm, mkey, Value::from_obj(sv2)));
    auto field_target = inst->load_field(vm, mkey);
    ASSERT_TRUE(field_target.has_value());
    EXPECT_TRUE(value_identical(*field_target, Value::from_obj(sv2)));

    // 静态槽持未戳闭包(lambda,无 defining class 戳):原值直读不绑定(方法性看戳不看值类型)。
    auto hkey = new_string(gc, "h");
    guard.push(hkey);
    auto lam = make_closure(gc, "h", 0);
    guard.push(lam);
    klass->set_field(hkey, Value::from_obj(lam));
    auto lambda_read = inst->load_field_bound(vm, hkey);
    ASSERT_TRUE(lambda_read.has_value());
    EXPECT_TRUE(value_identical(*lambda_read, Value::from_obj(lam))); // === 原闭包,无 ObjBoundMethod 包装

    // 真字段遮蔽同名类表成员:this.m = 9 走 store_field 后读到字段值。
    EXPECT_TRUE(inst->store_field(vm, mkey, Value::from_int(9)));
    auto shadowed_read = inst->load_field_bound(vm, mkey);
    ASSERT_TRUE(shadowed_read.has_value());
    EXPECT_EQ(shadowed_read->as_int(), 9);

    // 全链 miss:委托类协议,随类措辞 fail(UndefinedProperty,消息含宿主类 debug 渲染;
    // 文案随宿主类,不在实例侧自持)。
    auto miss = new_string(gc, "missing");
    guard.push(miss);
    EXPECT_FALSE(inst->load_field_bound(vm, miss).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("<class Foo> has no member 'missing'"));
}

// store_field:实例字段动态 set,永不失败(恒 true;false ⟺ 已 fail)。
TEST(ObjInstance, StoreFieldDynamicSet) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto klass = make_class(gc, "Foo");
    guard.push(klass);
    auto inst = make_instance(gc, klass);
    guard.push(inst);

    auto k = new_string(gc, "x");
    guard.push(k);
    EXPECT_TRUE(inst->store_field(vm, k, Value::from_int(1))); // 即创建
    auto created_read = inst->load_field_bound(vm, k);
    ASSERT_TRUE(created_read.has_value());
    EXPECT_EQ(created_read->as_int(), 1);
    EXPECT_TRUE(inst->store_field(vm, k, Value::from_int(2))); // 原槽更新
    auto updated_read = inst->load_field_bound(vm, k);
    ASSERT_TRUE(updated_read.has_value());
    EXPECT_EQ(updated_read->as_int(), 2);
}
