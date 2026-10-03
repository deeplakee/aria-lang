// ObjRange(纯值表示/内容哈希/equals/load_field_bound 绑定)与 ObjRangeIterator(区间当前值游标,
// 无源对象)的对象层测试;错误白盒取件与 GC 守卫形态同 test_objmap/test_objiterator。
#include <gtest/gtest.h>

#include <string>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjRange.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::i64;
using aria::new_range;
using aria::new_range_iterator;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjNativeFn;
using aria::ObjRange;
using aria::ObjRangeIterator;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_as_obj;
using aria::usize;
using aria::Value;
using aria::value_identical;

namespace {

    // 建 range 并入根(白色对象守卫,对标 test_objmap 的 make_map)。
    ObjRange* make_range(GC& gc, GC::Guard& guard, const i64 from, const i64 to, const bool is_exclusive) {
        auto range = new_range(gc, from, to, is_exclusive);
        guard.push(range);
        return range;
    }

    // 建无上界 range 并入根(白色对象守卫)。
    ObjRange* make_unbounded_range(GC& gc, GC::Guard& guard, const i64 from) {
        auto range = new_range(gc, from);
        guard.push(range);
        return range;
    }

    // 建迭代器并入根(迭代器白色,守卫后检视/推进)。
    ObjRangeIterator* make_range_iterator(GC& gc, GC::Guard& guard, ObjRange* range) {
        auto iter = new_range_iterator(gc, range);
        guard.push(iter);
        return iter;
    }

    // next 取件:耗尽即失败(调用方保证未越界)。
    i64 take_next(AriaVM& vm, ObjRangeIterator* iter) {
        const auto value = iter->next(vm);
        EXPECT_TRUE(value.has_value());
        return value ? value->as_int() : -1;
    }

    // 白盒取件:从挂起错误寄存器取出 ObjException,拆 (码, 烘焙消息) 两件
    //(对标 test_objlist/test_objmap 同名 helper)。
    Pair<ErrorCode, String> take_pending_error(AriaVM& vm) {
        auto payload = vm.current_context()->take_error();
        EXPECT_TRUE(payload.has_value());
        const auto ex = try_as_obj<ObjException>(*payload);
        EXPECT_NE(ex, nullptr);
        return {ex->code(), String{ex->message()->view()}};
    }

} // namespace

// ---- ObjRange 本体 ----

TEST(ObjRange, Basics) {
    GC   gc;
    auto guard = gc.make_guard();
    auto range = make_range(gc, guard, 0, 10, false);
    EXPECT_TRUE(range->is<ObjRange>());
    EXPECT_EQ(range->type(), aria::ObjType::RANGE);
    EXPECT_EQ(range->type_name(), aria::StringView{"Range"});
    EXPECT_EQ(range->from(), 0);
    EXPECT_EQ(range->to(), 10);
    EXPECT_FALSE(range->is_exclusive());
    EXPECT_EQ(range->size(), sizeof(ObjRange)); // 壳定长,无外挂 buffer
}

TEST(ObjRange, ContentHashIsDeterministic) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_range(gc, guard, 0, 10, false);
    auto b     = make_range(gc, guard, 0, 10, false);
    EXPECT_EQ(a->hash(), b->hash()); // 同内容同哈希(内容哈希型)
    // 不同内容(端点/含否上界翻转)哈希可区分(选定值实测;内容哈希使交换/翻转不入同桶键)。
    auto exclusive = make_range(gc, guard, 0, 10, true);
    auto shifted   = make_range(gc, guard, 1, 10, false);
    EXPECT_NE(a->hash(), exclusive->hash());
    EXPECT_NE(a->hash(), shifted->hash());
}

TEST(ObjRange, EqualsIsContent) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_range(gc, guard, 0, 10, false);
    auto b     = make_range(gc, guard, 0, 10, false);
    EXPECT_TRUE(a->equals(b));
    EXPECT_TRUE(b->equals(a));

    auto exclusive = make_range(gc, guard, 0, 10, true);
    auto shifted   = make_range(gc, guard, 1, 10, false);
    auto narrowed  = make_range(gc, guard, 0, 9, false);
    EXPECT_FALSE(a->equals(exclusive)); // 含否上界不同
    EXPECT_FALSE(a->equals(shifted));   // 下界不同
    EXPECT_FALSE(a->equals(narrowed));  // 上界不同
    EXPECT_FALSE(a->equals(nullptr));
}

TEST(ObjRange, DebugReprMatchesSourceSpelling) {
    GC   gc;
    auto guard     = gc.make_guard();
    auto inclusive = make_range(gc, guard, 0, 10, false);
    auto exclusive = make_range(gc, guard, 0, 10, true);
    auto negatives = make_range(gc, guard, -2, 3, false);
    auto reversed  = make_range(gc, guard, 5, 3, false);
    EXPECT_EQ(inclusive->debug_repr(), "0..10");
    EXPECT_EQ(exclusive->debug_repr(), "0...10");
    EXPECT_EQ(negatives->debug_repr(), "-2..3");
    EXPECT_EQ(reversed->debug_repr(), "5..3"); // 倒序区间原样渲染两端点
}

// 无上界开区间:to_ 为空(类型即契约)、含否上界归一 false、debug_repr 渲染 from..。
TEST(ObjRange, UnboundedBasics) {
    GC   gc;
    auto guard = gc.make_guard();
    auto range = make_unbounded_range(gc, guard, 5);
    EXPECT_FALSE(range->to().has_value());
    EXPECT_FALSE(range->is_exclusive()); // ctor 归一:5.. 与 5... 同义
    EXPECT_EQ(range->debug_repr(), "5..");
    EXPECT_EQ(range->from(), 5);
    EXPECT_EQ(range->size(), sizeof(ObjRange));
}

// 无上界 equals/hash:同为无界且起点相等才等;与有界(含 0 端点)可区分。
TEST(ObjRange, UnboundedEqualsAndHash) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_unbounded_range(gc, guard, 3);
    auto b     = make_unbounded_range(gc, guard, 3);
    auto other = make_unbounded_range(gc, guard, 4);
    auto to_it = make_range(gc, guard, 3, 0, false);
    auto excl  = make_range(gc, guard, 3, 0, true);
    EXPECT_TRUE(a->equals(b));
    EXPECT_FALSE(a->equals(other));
    EXPECT_FALSE(a->equals(to_it)); // 无界 vs 有界 ..0
    EXPECT_FALSE(a->equals(excl));  // 无界 vs 有界 ...0
    EXPECT_NE(a->hash(), to_it->hash());
}

// ---- 命名成员协议(load_field_bound → VM 的 Range bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根。
TEST(ObjRange, BootstrapClassContract) {
    AriaVM vm;
    auto*  range_class = vm.range_class();
    ASSERT_NE(range_class, nullptr);
    EXPECT_EQ(range_class->name()->view(), "Range");
    EXPECT_EQ(range_class->superclass(), vm.object_class());
}

// 命中恒绑定:bound 的 receiver 是本 range、method 是类表内的原生函数 iter。
TEST(ObjRange, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 10, false);
    auto   bound = range->load_field_bound(vm, new_string(gc, "iter"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_as_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(range)));
    const auto native = method->method().as_obj()->try_as<ObjNativeFn>();
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "iter");
}

// miss:类措辞 fail 随协议透传(与实例路径同文案形)。
TEST(ObjRange, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 10, false);
    EXPECT_FALSE(range->load_field_bound(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class Range> has no member 'nope'");
}

// stress collect 后类链仍解析:bootstrap 类经寄存器组根、方法原生经类链 field_ 表级联标根。
TEST(ObjRange, BootstrapSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       range   = make_range(gc, guard, 0, 10, false);
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    auto bound = range->load_field_bound(vm, new_string(gc, "iter"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_as_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "iter"); // name() 经 bound 的原生取名,存活即链完好
}

// ---- ObjRangeIterator(区间当前值游标,无源对象) ----

// 含上界:0..5 产出 0..5 共 6 值,取尽后 has_next 恒 false。
TEST(ObjRangeIterator, InclusiveYieldsAllValues) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 5, false);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = 0; expected <= 5; ++expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
    EXPECT_FALSE(iter->has_next());
}

// 不含上界:0...5 产出 0..4 共 5 值。
TEST(ObjRangeIterator, ExclusiveExcludesUpper) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 5, true);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = 0; expected < 5; ++expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
    EXPECT_FALSE(iter->has_next());
}

// 空区间零迭代:仅 from==to 且不含上界(5...5)首问即 false。
TEST(ObjRangeIterator, EmptyExclusiveZeroRounds) {
    AriaVM vm;
    auto&  gc        = vm.gc();
    auto   guard     = gc.make_guard();
    auto   half_open = make_range(gc, guard, 5, 5, true);
    auto   it        = make_range_iterator(gc, guard, half_open);
    EXPECT_FALSE(it->has_next());
}

// 倒序:from>to 构造期定向,含上界 10..1 产出 10→1。
TEST(ObjRangeIterator, ReversedInclusiveYieldsDescending) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 10, 1, false);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = 10; expected >= 1; --expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
    EXPECT_FALSE(iter->has_next());
}

// 倒序不含上界:10...1 产出 10→2(递减到 to+1)。
TEST(ObjRangeIterator, ReversedExclusiveExcludesLower) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 10, 1, true);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = 10; expected >= 2; --expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
    EXPECT_FALSE(iter->has_next());
}

// 单值区间:5..5 恰一个 5(含上界的 from==to)。
TEST(ObjRangeIterator, SingleValueInclusive) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 5, 5, false);
    auto   iter  = make_range_iterator(gc, guard, range);
    ASSERT_TRUE(iter->has_next());
    EXPECT_EQ(take_next(vm, iter), 5);
    EXPECT_FALSE(iter->has_next());
}

// 耗尽后 next fail-fast(IterationExhausted):forIn 靠 has_next 把关,仅手写滥用触此。
TEST(ObjRangeIterator, NextPastEndFailsFast) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 2, false);
    auto   iter  = make_range_iterator(gc, guard, range);
    take_next(vm, iter);
    take_next(vm, iter);
    take_next(vm, iter);
    const auto value = iter->next(vm);
    EXPECT_FALSE(value.has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::IterationExhausted);
    EXPECT_EQ(message, "Runtime: IterationExhausted iterator exhausted");
}

// 双迭代器互不串扰:同一 range 铸两个迭代器,游标独立(标量各自拷贝)。
TEST(ObjRangeIterator, TwoIteratorsCursorsIndependent) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_range(gc, guard, 0, 4, false);
    auto   it1   = make_range_iterator(gc, guard, range);
    auto   it2   = make_range_iterator(gc, guard, range);
    EXPECT_EQ(take_next(vm, it1), 0);
    EXPECT_EQ(take_next(vm, it1), 1);
    EXPECT_EQ(take_next(vm, it2), 0); // it2 游标未受 it1 推进影响
}

// 无上界开区间:has_next 恒真、方向恒正序,连续推进递增(取样 1000 次仍无尽)。
TEST(ObjRangeIterator, UnboundedHasNextAlwaysTrue) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_unbounded_range(gc, guard, 0);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = 0; expected < 1000; ++expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
    EXPECT_TRUE(iter->has_next());
}

// 无上界起点可为负:-2.. 产出 -2, -1, 0, ...(取样校验)。
TEST(ObjRangeIterator, UnboundedFromNegative) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   range = make_unbounded_range(gc, guard, -2);
    auto   iter  = make_range_iterator(gc, guard, range);
    for (i64 expected = -2; expected < 100; ++expected) {
        ASSERT_TRUE(iter->has_next());
        EXPECT_EQ(take_next(vm, iter), expected);
    }
}

// stress collect 下迭代器存活:标量自足无子对象,guard 持根跑完整个迭代。
TEST(ObjRangeIterator, StressCollectKeepsIterator) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       range   = make_range(gc, guard, 0, 6, false);
    auto       iter    = make_range_iterator(gc, guard, range);
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    i64 sum = 0;
    while (iter->has_next()) {
        sum += take_next(vm, iter);
    }
    EXPECT_EQ(sum, 21); // 0+1+..+6
}
