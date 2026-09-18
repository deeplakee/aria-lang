// ObjIterator 基类(引擎缝 + load_field 绑定)与 ObjListIterator(游标推进/越界 fail/trace)
// 的对象层测试;错误白盒取件同 test_objlist 的 take_pending_error 形态。
#include <gtest/gtest.h>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjList.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_list;
using aria::new_list_iterator;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjList;
using aria::ObjListIterator;
using aria::ObjNativeFn;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_obj;
using aria::Value;
using aria::value_identical;

namespace {

    // 建 list 并入根,返回后调用方自行 push 元素(trivial 分配不触 GC)。
    ObjList* make_list(GC& gc, GC::Guard& guard) {
        auto list = new_list(gc);
        guard.push(list);
        return list;
    }

    // 建串并入根(建时 stress collect 在串诞生前完成,push 前无 GC 点)。
    ObjString* make_string(GC& gc, GC::Guard& guard, const StringView src) {
        auto s = new_string(gc, src);
        guard.push(s);
        return s;
    }

    // 建 list 迭代器并入临时根(断言期存活;list 由调用方另守)。
    ObjListIterator* make_list_iterator(GC& gc, GC::Guard& guard, ObjList* list) {
        auto iter = new_list_iterator(gc, list);
        guard.push(iter);
        return iter;
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(引擎缝契约:next 越界 ⟺ 寄存器必有载荷)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.main_context().take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

// 基类契约:类型名单数 "Iterator"、debug_repr "<iterator>"(源类型不进文案)。
TEST(ObjIterator, BaseReprAndType) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   iter  = make_list_iterator(gc, guard, list);
    EXPECT_EQ(iter->type(), aria::ObjType::ITERATOR);
    EXPECT_EQ(iter->type_name(), aria::StringView{"Iterator"});
    EXPECT_EQ(iter->debug_repr(), "<iterator>");
    EXPECT_EQ(iter->to_string(), "<iterator>"); // 显示同文案
}

// 同一 list 的两个迭代器按身份判等(游标互不串扰的结构前提)。
TEST(ObjIterator, EqualsIsIdentity) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   a     = make_list_iterator(gc, guard, list);
    auto   b     = make_list_iterator(gc, guard, list);
    EXPECT_TRUE(a->equals(a));
    EXPECT_FALSE(a->equals(b));
    EXPECT_TRUE(value_identical(Value::from_obj(a), Value::from_obj(a)));
    EXPECT_FALSE(value_identical(Value::from_obj(a), Value::from_obj(b)));
}

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根。
TEST(ObjIterator, BootstrapClassContract) {
    AriaVM vm;
    auto*  iter_class = vm.iterator_class();
    ASSERT_NE(iter_class, nullptr);
    EXPECT_EQ(iter_class->name()->view(), "Iterator");
    EXPECT_EQ(iter_class->superclass(), vm.object_class());
}

// load_field 恒绑定:has_next 绑到本迭代器,method 是 Iterator 类表内的原生。
TEST(ObjIterator, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   iter  = make_list_iterator(gc, guard, list);
    auto   bound = iter->load_field(vm, new_string(gc, "has_next"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(iter)));
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "has_next");
}

// load_field miss:类措辞 fail 随协议透传。
TEST(ObjIterator, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   iter  = make_list_iterator(gc, guard, list);
    EXPECT_FALSE(iter->load_field(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class Iterator> has no member 'nope'");
}

// 游标推进:has_next/next 交替取尽,next 返回元素拷贝并推进。
TEST(ObjListIterator, CursorAdvancesToExhaustion) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    list->elements().push(Value::from_int(10));
    list->elements().push(Value::from_int(20));
    auto iter = make_list_iterator(gc, guard, list);
    EXPECT_TRUE(iter->has_next());
    auto first = iter->next(vm);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->as_int(), 10);
    EXPECT_TRUE(iter->has_next());
    auto second = iter->next(vm);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->as_int(), 20);
    EXPECT_FALSE(iter->has_next());
}

// 空表:has_next 恒 false;next 越界 fail-fast(IterationExhausted),取尽后 has_next 仍 false。
TEST(ObjListIterator, NextPastEndFailsFast) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   iter  = make_list_iterator(gc, guard, list);
    EXPECT_FALSE(iter->has_next());
    EXPECT_FALSE(iter->next(vm).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::IterationExhausted);
    EXPECT_EQ(message, "Runtime: IterationExhausted iterator exhausted");
    EXPECT_FALSE(iter->has_next());
}

// 双迭代器游标独立:一个取尽不影响另一个从头取。
TEST(ObjListIterator, TwoIteratorsCursorsIndependent) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    list->elements().push(Value::from_int(1));
    list->elements().push(Value::from_int(2));
    auto first  = make_list_iterator(gc, guard, list);
    auto second = make_list_iterator(gc, guard, list);
    ASSERT_TRUE(first->next(vm).has_value());
    ASSERT_TRUE(first->next(vm).has_value());
    EXPECT_FALSE(first->has_next()); // first 取尽
    EXPECT_TRUE(second->has_next()); // second 不受影响
    auto value = second->next(vm);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(value->as_int(), 1);
}

// stress GC:迭代器为唯一根,list 及其元素经 trace 级联存活;漏标即读已回收内存。
TEST(ObjListIterator, TraceStressKeepsSource) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&            gc   = vm.gc();
    ObjListIterator* iter = nullptr;
    {
        auto guard = gc.make_guard();
        auto list  = make_list(gc, guard);
        list->elements().push(Value::from_int(42));
        const auto s = make_string(gc, guard, "element string");
        list->elements().push(Value::from_obj(s));
        iter = make_list_iterator(gc, guard, list);
    }
    auto       guard   = gc.make_guard(iter);       // 只根迭代器:list 仅经 iter->trace 可达
    const auto trigger = new_string(gc, "trigger"); // stress collect
    guard.push(trigger);
    const auto first = iter->next(vm);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->as_int(), 42);
    const auto second = iter->next(vm);
    ASSERT_TRUE(second.has_value());
    const auto survived = aria::Object::try_as<ObjString>(second->as_obj());
    ASSERT_NE(survived, nullptr);
    EXPECT_EQ(survived->view(), "element string");
}
