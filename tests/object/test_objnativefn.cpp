#include <gtest/gtest.h>

#include "memory/GC.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
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
    bool noop_fn(aria::AriaVM& /*vm*/, aria::Span<Value> slots) {
        slots[0] = aria::Value::nil_val();
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
    EXPECT_EQ(nf->to_string(), "<fn sample>");
}

// 匿名重载:new_native_fn(gc, fn) 以 kAnonymousName("<anonymous>") 建名,渲染 `<fn <anonymous>>`。
// name_ 恒非空(ctor ASSERT),无 nullptr 空态。
TEST(ObjNativeFn, AnonymousUsesAnonymousName) {
    GC           gc;
    ObjNativeFn* nf    = new_native_fn(gc, noop_fn);
    auto         guard = gc.make_guard(nf);
    ASSERT_NE(nf->name(), nullptr);
    EXPECT_EQ(nf->name()->view(), kAnonymousName);
    EXPECT_EQ(nf->to_string(), "<fn <anonymous>>");
}

// kAnonymousName 与 ObjFunction 的 lambda 命名一致("<anonymous>")。
TEST(ObjNativeFn, AnonymousNameConstantValue) { EXPECT_EQ(kAnonymousName, StringView{"<anonymous>"}); }
