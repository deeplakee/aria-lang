#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::kAnonymousName;
using aria::new_native_fn;
using aria::new_string;
using aria::ObjNativeFn;
using aria::StringView;
using aria::Value;

namespace {

    // 叶子原生函数桩:不操作值栈,仅把 nil 写到返回槽 0,满足 NativeFn 契约。
    bool noop_fn(const aria::NativeCallInfo& info) {
        info.set_return_val(aria::Value::nil_val());
        return true;
    }

} // namespace

// 具名原生函数:name 传入非空,渲染 `<fn name>`。
TEST(ObjNativeFn, NamedRendersFnName) {
    GC           gc;
    auto         name  = new_string(gc, "sample");
    auto         guard = gc.make_guard(name); // name 是 weak root,跨 new_object 顶 maybe_collect 先保
    ObjNativeFn* nf    = new_native_fn(gc, name, noop_fn);
    EXPECT_TRUE(nf->is<ObjNativeFn>());
    EXPECT_EQ(nf->type(), aria::ObjType::NATIVE_FN);
    EXPECT_EQ(nf->name(), name); // intern 同指针
    EXPECT_EQ(nf->fn(), noop_fn);
    EXPECT_EQ(nf->repr(), "<fn sample>");
    EXPECT_EQ(nf->defining_class(), nullptr);
    EXPECT_FALSE(nf->is_method());
    EXPECT_FALSE(aria::is_method(Value::from_obj(nf)));
}

// 匿名重载:new_native_fn(gc, fn) 以 kAnonymousName("<anonymous>") 建名,渲染 `<fn <anonymous>>`。
// name_ 恒非空(ctor ASSERT),无 nullptr 空态。
TEST(ObjNativeFn, AnonymousUsesAnonymousName) {
    GC           gc;
    ObjNativeFn* nf    = new_native_fn(gc, noop_fn);
    auto         guard = gc.make_guard(nf);
    ASSERT_NE(nf->name(), nullptr);
    EXPECT_EQ(nf->name()->view(), kAnonymousName);
    EXPECT_EQ(nf->repr(), "<fn <anonymous>>");
}

// kAnonymousName 与 ObjFnProto 的 lambda 命名一致("<anonymous>")。
TEST(ObjNativeFn, AnonymousNameConstantValue) { EXPECT_EQ(kAnonymousName, StringView{"<anonymous>"}); }

// 只根化方法即可保活定义类;复制到其他类表不改变方法性或定义归属。
TEST(ObjNativeFn, DefiningClassSurvivesCollectAndMemberAssignment) {
    GC gc;
    gc.set_stress(true);
    ObjNativeFn* method = nullptr;
    {
        auto owner = aria::new_class(gc, "Owner", nullptr);
        auto guard = gc.make_guard(owner);
        method     = new_native_fn(gc, "method", noop_fn);
        method->set_defining_class(owner);
        owner->set_field(method->name(), Value::from_obj(method));
    }

    auto guard = gc.make_guard(method);
    gc.collect();
    ASSERT_NE(method->defining_class(), nullptr);
    EXPECT_EQ(method->defining_class()->name()->view(), "Owner");
    EXPECT_TRUE(method->is_method());
    EXPECT_TRUE(aria::is_method(Value::from_obj(method)));

    auto other = aria::new_class(gc, "Other", nullptr);
    guard.push(other);
    other->set_field(method->name(), Value::from_obj(method));
    gc.collect();
    EXPECT_NE(method->defining_class(), other);
    EXPECT_EQ(method->defining_class()->name()->view(), "Owner");
}
