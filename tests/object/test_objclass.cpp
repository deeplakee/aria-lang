#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::new_class;
using aria::new_closure;
using aria::new_function;
using aria::new_module;
using aria::new_string;
using aria::ObjClass;
using aria::ObjClosure;
using aria::ObjFunction;
using aria::ObjModule;
using aria::ObjString;
using aria::StringView;
using aria::u8;
using aria::usize;
using aria::Value;
using aria::value_identical;

namespace {

    // intern + 守卫 name(super 非空时一并守卫),再调 new_class。工厂不再替调用方守卫入参,
    // 故本助手显式守卫。返回的 cls 未根(守卫随函数退出释放),调用方跨 GC 点持有须自行守卫。
    ObjClass* make_class(GC& gc, StringView name, ObjClass* super = nullptr) {
        auto nm    = new_string(gc, name);
        auto guard = gc.make_guard(nm);
        if (super != nullptr) {
            guard.push(super); // super 跨 new_object 须根化
        }
        return new_class(gc, nm, super);
    }

    // 临时模块 + 具名函数:intern + 守卫均收口在助手内(机制测试不关心模块归属)。
    ObjFunction* make_function(GC& gc, StringView name, u8 arity) {
        auto m     = new_module(gc, StringView{"<script>"});
        auto guard = gc.make_guard(m);
        auto nm    = new_string(gc, name);
        guard.push(nm);
        return new_function(gc, m, nm, arity);
    }

    // 具名闭包:fn 经守卫跨 new_closure。返回的闭包未根,fn 经其可达。
    ObjClosure* make_closure(GC& gc, StringView name, u8 arity) {
        auto fn    = make_function(gc, name, arity);
        auto guard = gc.make_guard(fn);
        return new_closure(gc, fn);
    }

} // namespace

TEST(ObjClass, Basics) {
    GC   gc;
    auto name       = new_string(gc, "Foo");
    auto name_guard = gc.make_guard(name); // 工厂不再守卫入参:name 裸持跨 new_class
    auto cls        = new_class(gc, name, nullptr);
    EXPECT_TRUE(aria::Object::is<ObjClass>(cls));
    EXPECT_EQ(cls->type(), aria::ObjType::CLASS);
    EXPECT_EQ(cls->name(), name); // intern 同指针
    EXPECT_EQ(cls->superclass(), nullptr);
    EXPECT_EQ(cls->field().size(), 0u); // 惰性:建类即查,无表
    EXPECT_EQ(cls->init(), nullptr);    // ctor nullptr 态,MAKE_CLASS seed 前无构造器
}

TEST(ObjClass, SuperclassInjects) {
    GC   gc;
    auto super = make_class(gc, "Base");
    auto sg    = gc.make_guard(super);
    auto cls   = make_class(gc, "Foo", super);
    EXPECT_EQ(cls->superclass(), super);
}

TEST(ObjClass, UpsertAndFindOwnTable) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto k   = new_string(gc, "x");
    auto kg  = gc.make_guard(k);
    auto v   = new_string(gc, "a long static value string!!!");
    auto vg  = gc.make_guard(v);

    auto* slot = cls->upsert_field(k);
    ASSERT_NE(slot, nullptr);
    *slot = Value::from_obj(v); // upsert 零填充(V{} 非 nil),调用方覆写

    Value* found = cls->find_field(k);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, slot); // 同槽指针
    EXPECT_TRUE(value_identical(*found, Value::from_obj(v)));

    auto* again = cls->upsert_field(k); // 命中已有槽,value 保留
    EXPECT_EQ(again, slot);
    EXPECT_TRUE(value_identical(*again, Value::from_obj(v)));
    EXPECT_EQ(cls->field().size(), 1u);
}

TEST(ObjClass, FindMissReturnsNull) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    auto k   = new_string(gc, "nope");
    auto kg  = gc.make_guard(k);
    EXPECT_EQ(cls->find_field(k), nullptr);
}

// 读穿透:Sub 未声明 x -> find_field 命中 Super 表项的值槽(同槽指针);Super 原槽更新后
// Sub 再读即新值(fall-through 每次读链上当前值)。类上赋值的写遮蔽落接收类自身表,不经
// 此槽(见 ReshadowInsertsOwnKey)。
TEST(ObjClass, FindReadsThroughChain) {
    GC   gc;
    auto super = make_class(gc, "Base");
    auto sg    = gc.make_guard(super);
    auto sub   = make_class(gc, "Sub", super);
    sg.push(sub);

    auto  k    = new_string(gc, "x");
    auto  kg   = gc.make_guard(k);
    auto  v1   = new_string(gc, "a long static value string one");
    auto  vg   = gc.make_guard(v1);
    auto* slot = super->upsert_field(k);
    *slot      = Value::from_obj(v1);

    Value* found = sub->find_field(k);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, slot); // 同槽指针:穿透读到父表项
    EXPECT_TRUE(value_identical(*found, Value::from_obj(v1)));

    auto v2       = new_string(gc, "a long static value string two"); // 父表原槽更新
    auto v2g      = gc.make_guard(v2);
    *slot         = Value::from_obj(v2);
    Value* reread = sub->find_field(k);
    EXPECT_EQ(reread, slot);
    EXPECT_TRUE(value_identical(*reread, Value::from_obj(v2)));
}

// 写遮蔽:类上赋值 Sub.x = v(STORE_FIELD 类路径:沿链存在性检查命中后 upsert 接收类自身)
// 在子表新建遮蔽键,父表槽不受影响--与 Python/JS class attributes 一致。
TEST(ObjClass, ReshadowInsertsOwnKey) {
    GC   gc;
    auto super = make_class(gc, "Base");
    auto sg    = gc.make_guard(super);
    auto sub   = make_class(gc, "Sub", super);
    sg.push(sub);

    auto  k          = new_string(gc, "x");
    auto  kg         = gc.make_guard(k);
    auto  v1         = new_string(gc, "a long static value string one");
    auto  vg         = gc.make_guard(v1);
    auto* super_slot = super->upsert_field(k);
    *super_slot      = Value::from_obj(v1);

    auto  v2       = new_string(gc, "a long static value string two");
    auto  v2g      = gc.make_guard(v2);
    auto* sub_slot = sub->upsert_field(k); // 写遮蔽:类上赋值 Sub.x = v 落接收类自身表
    *sub_slot      = Value::from_obj(v2);

    Value* found = sub->find_field(k);
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found, sub_slot); // 子表新键遮蔽父表
    EXPECT_NE(found, super_slot);
    EXPECT_TRUE(value_identical(*found, Value::from_obj(v2)));

    Value* sfound = super->find_field(k);
    ASSERT_NE(sfound, nullptr);
    EXPECT_EQ(sfound, super_slot); // 父表槽未被波及(写遮蔽不外溢)
    EXPECT_TRUE(value_identical(*sfound, Value::from_obj(v1)));
}

TEST(ObjClass, ToString) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    EXPECT_EQ(cls->to_string(), "<class Foo>");
}

TEST(ObjClass, DebugRender) {
    GC   gc;
    auto cls = make_class(gc, "Foo");
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(cls)), "<class Foo>"); // 调试渲染同文案
}

// stress GC:sub 为唯一根,superclass 链(经 superclass_)、静态值(长串)/方法闭包(defining
// class 级联回 sub)/init 闭包(fn 持常量池长串)全部经 sub.trace 存活。守卫全部作用域弹出,
// 断言压在 trace 覆盖上(漏标即丢)。
TEST(ObjClass, TraceStressKeepsStaticsInitAndSuper) {
    GC gc;
    gc.set_stress(true);

    ObjClass*   super    = nullptr;
    ObjClass*   sub      = nullptr;
    ObjClosure* method   = nullptr;
    ObjClosure* init     = nullptr;
    ObjString*  mkey     = nullptr;
    ObjString*  vkey     = nullptr;
    ObjString*  val      = nullptr;
    ObjString*  constant = nullptr;
    {
        auto g = gc.make_guard();
        super  = make_class(gc, "Base"); // 建时 collect:name 经助手内守卫
        g.push(super);
        sub = make_class(gc, "Sub", super); // 建时 collect:super 经守卫存活
        g.push(sub);
        method = make_closure(gc, "m", 0); // 建时 collect:super/sub 经守卫存活
        g.push(method);
        method->set_defining_class(sub);
        init = make_closure(gc, "init", 0);
        g.push(init);
        sub->set_init(init);
        constant = new_string(gc, "a long constant string beyond sso padding"); // 建时 collect:在根者存活
        g.push(constant);
        init->function()->unit().add_constant(Value::from_obj(constant)); // push 走 trivial 分配不触 GC
        mkey = new_string(gc, "m");
        g.push(mkey);
        vkey = new_string(gc, "x");
        g.push(vkey);
        val = new_string(gc, "a long static value string!!!"); // 建时 collect:在根者存活
        g.push(val);
        auto* mslot = sub->upsert_field(mkey); // 建表/rehash 非 GC 点(trivial 分配)
        *mslot      = Value::from_obj(method);
        auto* vslot = sub->upsert_field(vkey);
        *vslot      = Value::from_obj(val);
        // 作用域退出:全部临时根弹出,super/method/init/val 此后仅经 sub.trace 可达
    }
    auto        guard   = gc.make_guard(sub);        // 只根 sub
    const auto  trigger = new_string(gc, "trigger"); // stress collect:全链经 sub.trace 存活
    auto        tg      = gc.make_guard(trigger);    // 触发串入根:不被下方 collect 回收
    const usize before  = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(sub->superclass(), super); // 父类经链标存活
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    EXPECT_EQ(sub->init(), init);
    EXPECT_EQ(method->defining_class(), sub);
    EXPECT_EQ(sub->field().size(), 2u);
    Value* vfound = sub->find_field(vkey);
    ASSERT_NE(vfound, nullptr);
    EXPECT_TRUE(value_identical(*vfound, Value::from_obj(val)));
}

// defining class 级联:类仅经方法闭包的 defining_class_ 可达(闭包为根),collect 后类存活
// --静态方法闭包经 Foo.m 上栈时类亡指针不悬垂(M5 计划 §2.6)。
TEST(ObjClass, DefiningClassSurvivesViaClosureTrace) {
    GC          gc;
    ObjClass*   cls    = nullptr;
    ObjClosure* method = nullptr;
    {
        auto cls_guard = gc.make_guard();
        cls            = make_class(gc, "K");
        cls_guard.push(cls);
        method       = make_closure(gc, "m", 0);
        auto m_guard = gc.make_guard(method);
        method->set_defining_class(cls);
        // 作用域退出:cls/method 的临时根全部弹出,二者此后仅经 method->defining_class_ 相连
    }
    auto        guard  = gc.make_guard(method); // 只根闭包
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(method->defining_class(), cls);
    EXPECT_EQ(cls->name()->view(), "K"); // 类经闭包 trace 存活,其 name_ 级联存活
}

// 未根类被 sweep。
TEST(ObjClass, UnrootedClassSwept) {
    GC gc;
    (void) make_class(gc, "orphan");
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}
