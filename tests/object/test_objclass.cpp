#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_class;
using aria::new_closure;
using aria::new_function;
using aria::new_module;
using aria::new_string;
using aria::ObjClass;
using aria::ObjClosure;
using aria::ObjException;
using aria::ObjFunction;
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
    // make_class 返回的未根指针;super 可空,make_guard 容空)。返回的 klass 未根
    //(守卫随函数退出释放),调用方跨 GC 点持有须自行守卫。
    ObjClass* make_class(GC& gc, const StringView name, ObjClass* super = nullptr) {
        auto guard = gc.make_guard(super);
        return new_class(gc, name, super);
    }

    // 临时模块 + 具名函数:守卫收口在助手内(机制测试不关心模块归属)。m 未根须自守:
    // 工厂内 intern name 与 new_object 均 GC 点。返回白色,调用方自守。
    ObjFunction* make_function(GC& gc, const StringView name, const u8 arity) {
        auto m     = new_module(gc, StringView{"<script>"});
        auto guard = gc.make_guard(m);
        return new_function(gc, m, name, arity, arity, false); // 无缺省,min_arity = arity
    }

    // 具名闭包:fn 经守卫跨 new_closure。返回的闭包未根,fn 经其可达。
    ObjClosure* make_closure(GC& gc, const StringView name, const u8 arity) {
        auto fn    = make_function(gc, name, arity);
        auto guard = gc.make_guard(fn);
        return new_closure(gc, fn);
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

TEST(ObjClass, Basics) {
    GC   gc;
    auto name  = new_string(gc, "Foo");
    auto guard = gc.make_guard(name); // 工厂不守卫入参:name 裸持跨 new_class
    auto klass = new_class(gc, name, nullptr);
    EXPECT_TRUE(aria::Object::is<ObjClass>(klass));
    EXPECT_EQ(klass->type(), aria::ObjType::CLASS);
    EXPECT_EQ(klass->name(), name); // intern 同指针
    EXPECT_EQ(klass->superclass(), nullptr);
    EXPECT_TRUE(klass->init().is_nil()); // ctor 自 super 派生:根态(super==nullptr)出厂 nil,由 bootstrap 经 set_field 设
}

TEST(ObjClass, SuperclassInjects) {
    GC   gc;
    auto super = make_class(gc, "Base");
    auto guard = gc.make_guard(super);
    auto klass = make_class(gc, "Foo", super);
    EXPECT_EQ(klass->superclass(), super);
}

// set_field(创建/更新本类自身表)+ load_field 直读:创建后可读、覆写后读新值。
TEST(ObjClass, SetFieldThenLoadOwnTable) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   klass = make_class(gc, "Foo");
    auto   k     = new_string(gc, "x");
    auto   guard = gc.make_guard(k);
    auto   v     = new_string(gc, "a long static value string!!!");
    guard.push(v);

    klass->set_field(k, Value::from_obj(v));

    auto initial_read = klass->load_field(vm, k);
    ASSERT_TRUE(initial_read.has_value());
    EXPECT_TRUE(value_identical(*initial_read, Value::from_obj(v)));

    auto v2 = new_string(gc, "a long static value string two");
    guard.push(v2);
    klass->set_field(k, Value::from_obj(v2)); // 覆写:原槽更新
    auto rewritten_read = klass->load_field(vm, k);
    ASSERT_TRUE(rewritten_read.has_value());
    EXPECT_TRUE(value_identical(*rewritten_read, Value::from_obj(v2)));
}

// 全链 miss:override 以类措辞就地 fail(nullopt ⟺ 已 fail),码 UndefinedProperty、
// 消息含类 debug 渲染。
TEST(ObjClass, LoadFieldMissFailsWithUndefinedProperty) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   klass = make_class(gc, "Foo");
    auto   guard = gc.make_guard(klass); // miss 的 fail 装箱是分配点:类须在根
    auto   k     = new_string(gc, "nope");
    guard.push(k);
    EXPECT_FALSE(klass->load_field(vm, k).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("<class Foo> has no member 'nope'"));
}

// 读穿透:Sub 未声明 x -> load_field 命中 Super 表项;Super 原槽更新后 Sub 再读即新值
// (fall-through 每次读链上当前值)。类上赋值的写遮蔽落接收类自身表(见 ReshadowInsertsOwnKey)。
TEST(ObjClass, LoadFieldReadsThroughChain) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   super = make_class(gc, "Base");
    auto   guard = gc.make_guard(super);
    auto   sub   = make_class(gc, "Sub", super);
    guard.push(sub);

    auto k = new_string(gc, "x");
    guard.push(k);
    auto v1 = new_string(gc, "a long static value string one");
    guard.push(v1);
    super->set_field(k, Value::from_obj(v1));

    auto through_read = sub->load_field(vm, k); // 穿透:子类读命中父表
    ASSERT_TRUE(through_read.has_value());
    EXPECT_TRUE(value_identical(*through_read, Value::from_obj(v1)));

    auto v2 = new_string(gc, "a long static value string two"); // 父表原槽更新
    guard.push(v2);
    super->set_field(k, Value::from_obj(v2));
    auto updated_read = sub->load_field(vm, k);
    ASSERT_TRUE(updated_read.has_value());
    EXPECT_TRUE(value_identical(*updated_read, Value::from_obj(v2)));
}

// 写遮蔽:类上赋值 Sub.x = v(store_field 命中链上名字后 set_field 落接收类自身表)
// 在子表新建遮蔽键,父表槽不受影响--与 Python/JS class attributes 一致。
TEST(ObjClass, ReshadowInsertsOwnKey) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   super = make_class(gc, "Base");
    auto   guard = gc.make_guard(super);
    auto   sub   = make_class(gc, "Sub", super);
    guard.push(sub);

    auto k = new_string(gc, "x");
    guard.push(k);
    auto v1 = new_string(gc, "a long static value string one");
    guard.push(v1);
    super->set_field(k, Value::from_obj(v1));

    auto v2 = new_string(gc, "a long static value string two");
    guard.push(v2);
    EXPECT_TRUE(sub->store_field(vm, k, Value::from_obj(v2))); // 写遮蔽落自身表

    auto shadowed_read = sub->load_field(vm, k);
    ASSERT_TRUE(shadowed_read.has_value());
    EXPECT_TRUE(value_identical(*shadowed_read, Value::from_obj(v2))); // 子表新键遮蔽父表

    auto parent_read = super->load_field(vm, k);
    ASSERT_TRUE(parent_read.has_value());
    EXPECT_TRUE(value_identical(*parent_read, Value::from_obj(v1))); // 父表槽未被波及(写遮蔽不外溢)
}

TEST(ObjClass, ToString) {
    GC   gc;
    auto klass = make_class(gc, "Foo");
    EXPECT_EQ(klass->to_string(), "<class Foo>");
}

TEST(ObjClass, DebugRender) {
    GC   gc;
    auto klass = make_class(gc, "Foo");
    EXPECT_EQ(aria::format_value_debug(Value::from_obj(klass)), "<class Foo>"); // 调试渲染同文案
}

// stress GC:sub 为唯一根,superclass 链(经 superclass_)、静态值(长串)/方法闭包(defining
// class 级联回 sub)/init 闭包(fn 持常量池长串)全部经 sub.trace 存活。守卫全部作用域弹出,
// 断言压在 trace 覆盖上(漏标即丢)。
TEST(ObjClass, TraceStressKeepsStaticsInitAndSuper) {
    AriaVM vm;
    auto&  gc = vm.gc();
    gc.set_stress(true);

    ObjClass*   super    = nullptr;
    ObjClass*   sub      = nullptr;
    ObjClosure* method   = nullptr;
    ObjClosure* init     = nullptr;
    ObjString*  ikey     = nullptr;
    ObjString*  mkey     = nullptr;
    ObjString*  vkey     = nullptr;
    ObjString*  val      = nullptr;
    ObjString*  constant = nullptr;
    {
        auto guard = gc.make_guard();
        super      = make_class(gc, "Base"); // 建时 collect:name 经助手内守卫
        guard.push(super);
        sub = make_class(gc, "Sub", super); // 建时 collect:super 经守卫存活
        guard.push(sub);
        method = make_closure(gc, "m", 0); // 建时 collect:super/sub 经守卫存活
        guard.push(method);
        method->set_defining_class(sub);
        init = make_closure(gc, "init", 0);
        guard.push(init);
        ikey = new_string(gc, "init");
        guard.push(ikey);
        sub->set_field(ikey, Value::from_obj(init)); // 落表 + 同步 init_(set_field 单一写入口)
        constant = new_string(gc, "a long constant string beyond sso padding"); // 建时 collect:在根者存活
        guard.push(constant);
        init->function()->unit().add_constant(Value::from_obj(constant)); // push 走 trivial 分配不触 GC
        mkey = new_string(gc, "m");
        guard.push(mkey);
        vkey = new_string(gc, "x");
        guard.push(vkey);
        val = new_string(gc, "a long static value string!!!"); // 建时 collect:在根者存活
        guard.push(val);
        sub->set_field(mkey, Value::from_obj(method)); // 建表/rehash 非 GC 点(trivial 分配)
        sub->set_field(vkey, Value::from_obj(val));
        // 作用域退出:全部临时根弹出,super/method/init/val 此后仅经 sub.trace 可达
    }
    auto       guard   = gc.make_guard(sub);        // 只根 sub
    const auto trigger = new_string(gc, "trigger"); // stress collect:全链经 sub.trace 存活
    guard.push(trigger);                            // 触发串入根:不被下方 collect 回收
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(sub->superclass(), super); // 父类经链标存活
    EXPECT_EQ(constant->view(), "a long constant string beyond sso padding");
    EXPECT_TRUE(value_identical(sub->init(), Value::from_obj(init))); // set_field 同步:表槽/init_ 一致
    EXPECT_EQ(method->defining_class(), sub);
    auto vfound = sub->load_field(vm, vkey);
    ASSERT_TRUE(vfound.has_value());
    EXPECT_TRUE(value_identical(*vfound, Value::from_obj(val)));
}

// defining class 级联:类仅经方法闭包的 defining_class_ 可达(闭包为根),collect 后类存活
// --静态方法闭包经 Foo.m 上栈时类亡指针不悬垂（defining_class_ 戳住闭包，见 .claude/rules/object.md）。
TEST(ObjClass, DefiningClassSurvivesViaClosureTrace) {
    GC          gc;
    ObjClass*   klass  = nullptr;
    ObjClosure* method = nullptr;
    {
        auto guard = gc.make_guard();
        klass      = make_class(gc, "K");
        guard.push(klass);
        method = make_closure(gc, "m", 0);
        guard.push(method);
        method->set_defining_class(klass);
        // 作用域退出:klass/method 的临时根全部弹出,二者此后仅经 method->defining_class_ 相连
    }
    auto        guard  = gc.make_guard(method); // 只根闭包
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before);
    EXPECT_EQ(method->defining_class(), klass);
    EXPECT_EQ(klass->name()->view(), "K"); // 类经闭包 trace 存活,其 name_ 级联存活
}

// 未根类被 sweep。
TEST(ObjClass, UnrootedClassSwept) {
    GC gc;
    make_class(gc, "orphan");
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// ---- 成员访问协议 override ----

// 构造期 init 派生:ObjClass 构造函数自 super 派生 init_ --super 非空出厂即继承父 init_
// 当前值(快照语义:此后父 init 变更不传导);Object 根态(super==nullptr)出厂 nil,由
// bootstrap 经 set_field 设(VM 级行为由
// InstantiateNoInitUsesSeededNativeInit/InheritanceOverrideAndSuperCall 钉)。
TEST(ObjClass, CtorDerivesInitFromSuper) {
    GC   gc;
    auto guard = gc.make_guard();

    auto base = make_class(gc, "Base");
    guard.push(base);
    auto pre = make_class(gc, "Pre", base); // 建于父 init 设值前:ctor 快照为 nil
    guard.push(pre);

    auto base_init = make_closure(gc, "init", 0);
    guard.push(base_init);
    auto init_key = new_string(gc, "init");
    guard.push(init_key);
    base->set_field(init_key, Value::from_obj(base_init));

    auto sub = make_class(gc, "Sub", base); // ctor 自 super 派生:出厂即继承父 init 当前值
    guard.push(sub);
    EXPECT_TRUE(value_identical(sub->init(), Value::from_obj(base_init)));
    EXPECT_TRUE(pre->init().is_nil()); // 快照语义:base 后设 init 不传导回已建的子类

    auto root = make_class(gc, "Rootless", nullptr); // Object 根态:出厂 nil(bootstrap 经 set_field 设)
    guard.push(root);
    EXPECT_TRUE(root->init().is_nil());
}

// load_field 读穿透:沿链直读(静态值/方法闭包原样),全链 miss 以类措辞 fail
// (nullopt ⟺ 已 fail);不绑定不缓存(类路径无 this)。
TEST(ObjClass, LoadFieldProtocolReadsThroughChain) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto base = make_class(gc, "Base");
    guard.push(base);
    auto sub = make_class(gc, "Sub", base);
    guard.push(sub);

    auto bk = new_string(gc, "inherited");
    guard.push(bk);
    auto bv = new_string(gc, "base static value string!!!!!");
    guard.push(bv);
    base->set_field(bk, Value::from_obj(bv));

    // 读穿透:子类读命中父表槽。
    auto inherited_read = sub->load_field(vm, bk);
    ASSERT_TRUE(inherited_read.has_value());
    EXPECT_TRUE(value_identical(*inherited_read, Value::from_obj(bv)));

    auto sk = new_string(gc, "own");
    guard.push(sk);
    auto sv = new_string(gc, "sub static value string!!!!!!");
    guard.push(sv);
    sub->set_field(sk, Value::from_obj(sv));
    auto own_read = sub->load_field(vm, sk);
    ASSERT_TRUE(own_read.has_value());
    EXPECT_TRUE(value_identical(*own_read, Value::from_obj(sv)));

    // 全链 miss:类措辞 fail(UndefinedProperty,消息含类 debug 渲染)。
    auto miss = new_string(gc, "missing");
    guard.push(miss);
    EXPECT_FALSE(sub->load_field(vm, miss).has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("<class Sub> has no member 'missing'"));
}

// store_field 写遮蔽 + 动态新增 + init 同步:新名字落本类自身表;继承名新建遮蔽键、父表
// 不动;"init" 命中同步 init_(表槽/init_ 一致由对象自维护)。恒成功;fail 通道经 base 读
// 新名 miss 断言钉住(码 + 类措辞)。
TEST(ObjClass, StoreFieldShadowsCreatesAndSyncsInit) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();

    auto base = make_class(gc, "Base");
    guard.push(base);
    auto sub = make_class(gc, "Sub", base);
    guard.push(sub);

    auto bk = new_string(gc, "x");
    guard.push(bk);
    auto bv = new_string(gc, "base static value string!!!!!");
    guard.push(bv);
    base->set_field(bk, Value::from_obj(bv));

    // 命中继承名:落子表遮蔽键,父表不动。
    auto sv = new_string(gc, "sub static value string!!!!!!");
    guard.push(sv);
    EXPECT_TRUE(sub->store_field(vm, bk, Value::from_obj(sv)));
    auto shadowed_read = sub->load_field(vm, bk);
    ASSERT_TRUE(shadowed_read.has_value());
    EXPECT_TRUE(value_identical(*shadowed_read, Value::from_obj(sv))); // 读到遮蔽值
    // 父表原槽不波及:base 直读仍是原值。
    auto parent_read = base->load_field(vm, bk);
    ASSERT_TRUE(parent_read.has_value());
    EXPECT_TRUE(value_identical(*parent_read, Value::from_obj(bv)));

    // 新名字:动态新增落子表,读回即新值。
    auto nk = new_string(gc, "nope");
    guard.push(nk);
    EXPECT_TRUE(sub->store_field(vm, nk, Value::from_int(1)));
    auto newkey_read = sub->load_field(vm, nk);
    ASSERT_TRUE(newkey_read.has_value());
    EXPECT_EQ(newkey_read->as_int(), 1);

    // 父类读不到新名:读穿透全链 miss(顺带钉 fail 通道:码 + 类措辞)。
    auto parent_newname_read = base->load_field(vm, nk);
    EXPECT_FALSE(parent_newname_read.has_value());
    auto [code, msg] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_TRUE(msg.contains("<class Base> has no member 'nope'"));

    // "init" 命中同步 init_。
    auto init_key = new_string(gc, "init");
    guard.push(init_key);
    auto init = make_closure(gc, "init", 0);
    guard.push(init);
    base->set_field(init_key, Value::from_obj(init)); // 链上有 init 槽(set_field 亦同步 base 的 init_)
    auto init2 = make_closure(gc, "init2", 0);
    guard.push(init2);
    EXPECT_TRUE(sub->store_field(vm, init_key, Value::from_obj(init2))); // 继承名遮蔽
    EXPECT_TRUE(value_identical(sub->init(), Value::from_obj(init2)));   // init_ 同步为遮蔽值

    // 非可调用 init 亦放行(init Value 化):表槽/init_ 一致,实例化期由 call_value 兜底。
    EXPECT_TRUE(sub->store_field(vm, init_key, Value::from_int(5)));
    EXPECT_TRUE(value_identical(sub->init(), Value::from_int(5)));
}
