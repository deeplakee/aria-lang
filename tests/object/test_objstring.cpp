// ObjString 协议面(下标字节语义/不可变写/load_field 绑定)的对象层测试;错误白盒取件与
// GC 守卫形态同 test_objlist/test_objmap。迭代器(ObjStringIterator)在 test_objiterator.cpp。
#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::i64;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjNativeFn;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_obj;
using aria::Value;
using aria::value_identical;

namespace {

    // 建串并入根(建时 stress collect 在串诞生前完成,检视前无 GC 点)。
    ObjString* make_string(GC& gc, GC::Guard& guard, const StringView src) {
        auto s = new_string(gc, src);
        guard.push(s);
        return s;
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(协议 fail 契约:load 族 nullopt / store 族 false ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.main_context().take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

// ---- 下标协议(load_index 字节域 / store_index 不可变) ----

// 字节域读:整数键产出单字节 1-char string(与 len 同域,计划 D5)。
TEST(ObjString, LoadIndexYieldsSingleByteString) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hello");

    auto first = s->load_index(vm, Value::from_int(0));
    ASSERT_TRUE(first.has_value());
    const auto h = aria::Object::try_as<ObjString>(first->as_obj());
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->view(), "h");

    auto last = s->load_index(vm, Value::from_int(4));
    ASSERT_TRUE(last.has_value());
    const auto o = aria::Object::try_as<ObjString>(last->as_obj());
    ASSERT_NE(o, nullptr);
    EXPECT_EQ(o->view(), "o");
}

// 多字节序列的中间字节取该字节自身(字节契约的自然结果,非完整字符):
// "héllo" 的 é 是 2 字节(0xC3 0xA9),s[1]/s[2] 各取到其中一字节。
TEST(ObjString, LoadIndexMidSequenceByteYieldsItself) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "h\xC3\xA9llo");

    auto b1 = s->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(b1.has_value());
    const auto first_byte = aria::Object::try_as<ObjString>(b1->as_obj());
    ASSERT_NE(first_byte, nullptr);
    const StringView mid_byte{"\xC3", 1}; // é 的首字节(花括号内逗号会劈 EXPECT_EQ 宏,先落局部)
    EXPECT_EQ(first_byte->view(), mid_byte);
    EXPECT_EQ(first_byte->length(), 1u); // 单字节,非完整字符
}

TEST(ObjString, LoadIndexNonIntKeyFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    for (const Value key: {Value::from_f64(0.5), Value::nil_val(), Value::from_bool(true)}) {
        EXPECT_FALSE(s->load_index(vm, key).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::TypeMismatch);
        EXPECT_EQ(message,
                  "Runtime: TypeMismatch string index must be an integer, got " + String{aria::type_name(key)});
    }
}

TEST(ObjString, LoadIndexOutOfBoundsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi"); // 长度 2
    for (const i64 index: {2, -1}) {
        EXPECT_FALSE(s->load_index(vm, Value::from_int(index)).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::IndexOutOfBounds);
        EXPECT_EQ(message, aria::String{std::format("Runtime: IndexOutOfBounds string index {} out of range", index)});
    }
}

// string 不可变:下标写恒报错(定向文案,键值不检查)。
TEST(ObjString, StoreIndexAlwaysFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    EXPECT_FALSE(s->store_index(vm, Value::from_int(0), Value::from_obj(s)));
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch string does not support subscript assignment");
}

// ---- 命名成员协议(load_field → VM 的 String bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根(计划 D1)。
TEST(ObjString, BootstrapClassContract) {
    AriaVM vm;
    auto*  string_class = vm.string_class();
    ASSERT_NE(string_class, nullptr);
    EXPECT_EQ(string_class->name()->view(), "String");
    EXPECT_EQ(string_class->superclass(), vm.object_class());
}

// 命中恒绑定:bound 的 receiver 是本串、method 是类表内的原生函数 upper。
TEST(ObjString, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    auto   bound = s->load_field(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(s)));
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "upper");
}

// miss:类措辞 fail 随协议透传(与 list/map 同文案形)。
TEST(ObjString, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "hi");
    EXPECT_FALSE(s->load_field(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class String> has no member 'nope'");
}

// stress collect 后类链仍解析:bootstrap 类经寄存器组根、方法原生经类链 field_ 表级联标根。
TEST(ObjString, BootstrapSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       s       = make_string(gc, guard, "hi");
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    auto bound = s->load_field(vm, new_string(gc, "upper"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "upper"); // name() 经 bound 的原生取名,存活即链完好
}
