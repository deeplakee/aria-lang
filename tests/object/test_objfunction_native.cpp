#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::GC;
using aria::kAnonymousName;
using aria::new_function;
using aria::new_string;
using aria::ObjFunction;
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
TEST(ObjFunctionNative, NamedRendersFnName) {
    GC           gc;
    auto         name     = new_string(gc, "sample");
    auto         guard    = gc.make_guard(name); // name 是 weak root,跨 new_object 顶 maybe_collect 先保
    ObjFunction* function = new_function(gc, name, noop_fn);
    EXPECT_TRUE(function->is<ObjFunction>());
    EXPECT_TRUE(function->is_native());
    EXPECT_EQ(function->type_name(), "Function");
    EXPECT_EQ(function->type(), aria::ObjType::FUNCTION);
    EXPECT_EQ(function->name(), name); // intern 同指针
    EXPECT_EQ(function->fn(), noop_fn);
    EXPECT_EQ(function->repr(), "<fn sample>");
    EXPECT_EQ(function->defining_class(), nullptr);
    EXPECT_FALSE(function->is_method());
}

// 匿名重载:new_function(gc, native) 以 kAnonymousName("<anonymous>") 建名,渲染 `<fn <anonymous>>`。
// name_ 恒非空(ctor ASSERT),无 nullptr 空态。
TEST(ObjFunctionNative, AnonymousUsesAnonymousName) {
    GC           gc;
    ObjFunction* function = new_function(gc, noop_fn);
    auto         guard    = gc.make_guard(function);
    ASSERT_NE(function->name(), nullptr);
    EXPECT_EQ(function->name()->view(), kAnonymousName);
    EXPECT_EQ(function->repr(), "<fn <anonymous>>");
}

// kAnonymousName 与 ObjFnProto 的 lambda 命名一致("<anonymous>")。
TEST(ObjFunctionNative, AnonymousNameConstantValue) { EXPECT_EQ(kAnonymousName, StringView{"<anonymous>"}); }

// 只根化方法即可保活定义类;复制到其他类表不改变方法性或定义归属。
TEST(ObjFunctionNative, DefiningClassSurvivesCollectAndMemberAssignment) {
    GC gc;
    gc.set_stress(true);
    ObjFunction* method = nullptr;
    {
        auto owner = aria::new_class(gc, "Owner", nullptr);
        auto guard = gc.make_guard(owner);
        method     = new_function(gc, "method", noop_fn);
        method->set_defining_class(owner);
        owner->set_field(method->name(), Value::from_obj(method));
    }

    auto guard = gc.make_guard(method);
    gc.collect();
    ASSERT_NE(method->defining_class(), nullptr);
    EXPECT_EQ(method->defining_class()->name()->view(), "Owner");
    EXPECT_TRUE(method->is_method());

    auto other = aria::new_class(gc, "Other", nullptr);
    guard.push(other);
    other->set_field(method->name(), Value::from_obj(method));
    gc.collect();
    EXPECT_NE(method->defining_class(), other);
    EXPECT_EQ(method->defining_class()->name()->view(), "Owner");
}
