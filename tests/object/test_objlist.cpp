#include <gtest/gtest.h>

#include <format>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjList.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjRange.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::i64;
using aria::new_list;
using aria::new_range;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjList;
using aria::ObjNativeFn;
using aria::ObjString;
using aria::Pair;
using aria::String;
using aria::StringView;
using aria::try_obj;
using aria::usize;
using aria::Value;
using aria::value_equal;
using aria::value_identical;

namespace {

    // 建串并入根(建时 stress collect 在串诞生前完成,push 前无 GC 点)。
    ObjString* make_string(GC& gc, GC::Guard& guard, const StringView src) {
        auto s = new_string(gc, src);
        guard.push(s);
        return s;
    }

    // 建 list 并入根,返回后调用方自行 push 元素(trivial 分配不触 GC)。
    ObjList* make_list(GC& gc, GC::Guard& guard) {
        auto list = new_list(gc);
        guard.push(list);
        return list;
    }

    // 建 [10..50] 五元素 list 并入根(切片测试用)。
    ObjList* make_five(GC& gc, GC::Guard& guard) {
        auto        list  = make_list(gc, guard);
        const Value src[] = {Value::from_int(10), Value::from_int(20), Value::from_int(30), Value::from_int(40),
                             Value::from_int(50)};
        list->elements().copy_from(src);
        return list;
    }

    // 切片取件:产出新 list 的元素逐个比对(调用方保证成功)。
    void expect_slice(AriaVM& vm, ObjList* list, aria::ObjRange* range, const aria::List<i64>& expected) {
        const auto result = list->load_index(vm, Value::from_obj(range));
        ASSERT_TRUE(result.has_value());
        const auto sliced = try_obj<ObjList>(*result);
        ASSERT_NE(sliced, nullptr);
        ASSERT_EQ(sliced->elements().size(), expected.size());
        for (usize index = 0; index < expected.size(); ++index) {
            EXPECT_EQ(sliced->elements()[index].as_int(), expected[index]);
        }
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

TEST(ObjList, Basics) {
    GC   gc;
    auto guard = gc.make_guard();
    auto list  = make_list(gc, guard);
    EXPECT_TRUE(aria::Object::is<ObjList>(list));
    EXPECT_EQ(list->type(), aria::ObjType::LIST);
    EXPECT_EQ(list->type_name(), aria::StringView{"List"});
    EXPECT_EQ(list->elements().size(), 0u);
    EXPECT_EQ(list->debug_repr(), "[]"); // 空 list 字面量形态
}

TEST(ObjList, FillKeepsOrder) {
    GC          gc;
    auto        guard = gc.make_guard();
    auto        s     = make_string(gc, guard, "elem");
    auto        list  = make_list(gc, guard);
    const Value src[] = {Value::from_int(1), Value::from_obj(s), Value::nil_val(), Value::from_bool(true)};
    list->elements().copy_from(src);
    ASSERT_EQ(list->elements().size(), 4u);
    EXPECT_EQ(list->elements()[0].as_int(), 1);
    EXPECT_TRUE(value_identical(list->elements()[1], Value::from_obj(s)));
    EXPECT_TRUE(list->elements()[2].is_nil());
    EXPECT_EQ(list->elements()[3].as_bool(), true);
}

TEST(ObjList, EqualsIsContentRecursive) {
    GC   gc;
    auto guard = gc.make_guard();
    auto s     = make_string(gc, guard, "a");

    // 同内容不同对象:逐元素相等。
    auto lhs = make_list(gc, guard);
    lhs->elements().push(Value::from_int(1));
    lhs->elements().push(Value::from_obj(s));
    auto rhs = make_list(gc, guard);
    rhs->elements().push(Value::from_int(1));
    rhs->elements().push(Value::from_obj(s));
    EXPECT_TRUE(lhs->equals(rhs));
    EXPECT_TRUE(value_equal(Value::from_obj(lhs), Value::from_obj(rhs)));      // == 走 equals 虚分派
    EXPECT_FALSE(value_identical(Value::from_obj(lhs), Value::from_obj(rhs))); // === 恒指针

    // 长度不同 / 内容不同。
    auto shorter = make_list(gc, guard);
    shorter->elements().push(Value::from_int(1));
    EXPECT_FALSE(lhs->equals(shorter));
    auto differing = make_list(gc, guard);
    differing->elements().push(Value::from_int(2));
    differing->elements().push(Value::from_obj(s));
    EXPECT_FALSE(lhs->equals(differing));

    // 嵌套递归:外层各自持有内容相等的内层 list。
    auto inner_a = make_list(gc, guard);
    inner_a->elements().push(Value::from_int(2));
    auto inner_b = make_list(gc, guard);
    inner_b->elements().push(Value::from_int(2));
    auto outer_a = make_list(gc, guard);
    outer_a->elements().push(Value::from_obj(inner_a));
    auto outer_b = make_list(gc, guard);
    outer_b->elements().push(Value::from_obj(inner_b));
    EXPECT_TRUE(outer_a->equals(outer_b));

    // 跨类型恒 false。
    EXPECT_FALSE(lhs->equals(s));
}

TEST(ObjList, DebugRepr) {
    GC   gc;
    auto guard = gc.make_guard();
    auto s     = make_string(gc, guard, "ab");
    auto inner = make_list(gc, guard);
    inner->elements().push(Value::from_int(2));
    auto list = make_list(gc, guard);
    list->elements().push(Value::from_int(1));
    list->elements().push(Value::from_obj(s));
    list->elements().push(Value::nil_val());
    list->elements().push(Value::from_obj(inner));
    // 嵌套字符串带引号(debug 形),嵌套 list 递归。
    EXPECT_EQ(list->debug_repr(), "[1, \"ab\", nil, [2]]");
    EXPECT_EQ(list->to_string(), "[1, \"ab\", nil, [2]]");                         // 显示同文案
    EXPECT_EQ(aria::format_value(Value::from_obj(list)), "[1, \"ab\", nil, [2]]"); // str/print 位
}

// 环防护:自引用/互环在渲染路径上重遇即截断 "[...]"(PrintGuard,防无限递归栈溢出)。
TEST(ObjList, DebugReprSelfCycleTruncates) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_list(gc, guard);
    a->elements().push(Value::from_int(1));
    a->elements().push(Value::from_obj(a));
    EXPECT_EQ(a->debug_repr(), "[1, [...]]");
    EXPECT_EQ(aria::format_value(Value::from_obj(a)), "[1, [...]]"); // print/str 位经 to_string 同路
    EXPECT_EQ(a->debug_repr(), "[1, [...]]");                        // 前次守卫已出栈,再次渲染不受影响
}

TEST(ObjList, DebugReprMutualCycleTruncates) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_list(gc, guard);
    auto b     = make_list(gc, guard);
    a->elements().push(Value::from_int(1));
    a->elements().push(Value::from_obj(b));
    b->elements().push(Value::from_obj(a));
    EXPECT_EQ(a->debug_repr(), "[1, [[...]]]"); // a -> b -> a:在 b 内重遇 a 截断
    EXPECT_EQ(b->debug_repr(), "[[1, [...]]]"); // b -> a -> b:在 a 内重遇 b 截断
}

// 守卫出栈正确性:同一非环内层 list 渲染两次,第二次不得误判环。
TEST(ObjList, DebugReprSharedElementRendersTwice) {
    GC   gc;
    auto guard = gc.make_guard();
    auto inner = make_list(gc, guard);
    inner->elements().push(Value::from_int(7));
    auto outer = make_list(gc, guard);
    outer->elements().push(Value::from_obj(inner));
    outer->elements().push(Value::from_obj(inner));
    EXPECT_EQ(outer->debug_repr(), "[[7], [7]]");
}

// ---- equals 环闭合(EqualGuard) ----

// 同指针快速路径先于环闭合链:自环 == 自身不进链。
TEST(ObjList, EqualsSelfCycleFastPath) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_list(gc, guard);
    a->elements().push(Value::from_obj(a));
    EXPECT_TRUE(a->equals(a));
    EXPECT_TRUE(value_equal(Value::from_obj(a), Value::from_obj(a)));
}

// 互环判等走余归纳:a=[b], b=[a], c=[c] 展开为同一棵无限树,两两判真;链摘净后重跑一致。
TEST(ObjList, EqualsMutualCycleCoinductive) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_list(gc, guard);
    auto b     = make_list(gc, guard);
    auto c     = make_list(gc, guard);
    a->elements().push(Value::from_obj(b));
    b->elements().push(Value::from_obj(a));
    c->elements().push(Value::from_obj(c));
    EXPECT_TRUE(a->equals(b));
    EXPECT_TRUE(value_equal(Value::from_obj(a), Value::from_obj(c)));
    EXPECT_TRUE(value_equal(Value::from_obj(b), Value::from_obj(c)));
    EXPECT_TRUE(a->equals(b));
}

// 环闭合不吞差异:环同尾异判 false,环对非环判 false。
TEST(ObjList, EqualsCycleDifferenceStillSeen) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_list(gc, guard);
    auto b     = make_list(gc, guard);
    a->elements().push(Value::from_obj(a));
    a->elements().push(Value::from_int(1));
    b->elements().push(Value::from_obj(b));
    b->elements().push(Value::from_int(2));
    EXPECT_FALSE(value_equal(Value::from_obj(a), Value::from_obj(b)));
    auto self1 = make_list(gc, guard);
    self1->elements().push(Value::from_obj(self1));
    auto two = make_list(gc, guard);
    two->elements().push(Value::from_int(2));
    EXPECT_FALSE(value_equal(Value::from_obj(self1), Value::from_obj(two)));
}

// stress GC:list 为唯一根,元素长串经 elements_.trace 存活;漏标即丢。
TEST(ObjList, TraceStressKeepsElements) {
    GC gc;
    gc.set_stress(true);
    ObjList*   list = nullptr;
    ObjString* elem = nullptr;
    {
        auto guard = gc.make_guard();
        elem       = make_string(gc, guard, "a long list element string!!!");
        list       = make_list(gc, guard);
        list->elements().push(Value::from_obj(elem));
        list->elements().push(Value::from_int(42));
        // 作用域退出:临时根弹出,elem 此后仅经 list.trace 可达
    }
    auto       guard   = gc.make_guard(list);       // 只根 list
    const auto trigger = new_string(gc, "trigger"); // stress collect:元素串经 trace 存活
    guard.push(trigger);
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    ASSERT_EQ(list->elements().size(), 2u);
    EXPECT_EQ(list->elements()[1].as_int(), 42);
    const auto survived = aria::Object::try_as<ObjString>(list->elements()[0].as_obj());
    ASSERT_NE(survived, nullptr);
    EXPECT_EQ(survived->view(), "a long list element string!!!");
}

// 未根 list 被 sweep(壳 + 元素值若无他根一并回收)。
TEST(ObjList, UnrootedListSwept) {
    GC gc;
    {
        auto guard = gc.make_guard();
        make_string(gc, guard, "orphan");
        make_list(gc, guard); // 守卫退出后双双失根
    }
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// ---- 下标协议(load_index/store_index) ----

TEST(ObjList, LoadIndexReads) {
    AriaVM      vm;
    auto&       gc    = vm.gc();
    auto        guard = gc.make_guard();
    auto        list  = make_list(gc, guard);
    const Value src[] = {Value::from_int(10), Value::from_int(20)};
    list->elements().copy_from(src);
    auto first = list->load_index(vm, Value::from_int(0));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->as_int(), 10);
    auto second = list->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->as_int(), 20);
}

TEST(ObjList, LoadIndexNonIntKeyFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    list->elements().push(Value::from_int(1));
    for (const Value key: {Value::from_f64(1.5), Value::nil_val(), Value::from_bool(true)}) {
        EXPECT_FALSE(list->load_index(vm, key).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::TypeMismatch);
        EXPECT_EQ(message, "Runtime: TypeMismatch list index must be an integer, got " + String{aria::type_name(key)});
    }
}

TEST(ObjList, LoadIndexOutOfBoundsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    list->elements().push(Value::from_int(1));
    list->elements().push(Value::from_int(2));
    for (const i64 index: {2, -3}) {
        EXPECT_FALSE(list->load_index(vm, Value::from_int(index)).has_value());
        const auto [code, message] = take_pending_error(vm);
        EXPECT_EQ(code, ErrorCode::IndexOutOfBounds);
        EXPECT_EQ(message, aria::String{std::format("Runtime: IndexOutOfBounds list index {} out of range", index)});
    }
}

// 负下标从尾计数:-1 = 末元素、-len = 首元素;-len-1 归一化后仍负即越界(文案报原始键值)。
TEST(ObjList, LoadIndexNegativeReadsFromTail) {
    AriaVM      vm;
    auto&       gc    = vm.gc();
    auto        guard = gc.make_guard();
    auto        list  = make_list(gc, guard);
    const Value src[] = {Value::from_int(10), Value::from_int(20), Value::from_int(30)};
    list->elements().copy_from(src);
    auto last = list->load_index(vm, Value::from_int(-1));
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->as_int(), 30);
    auto first = list->load_index(vm, Value::from_int(-3));
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->as_int(), 10);
    EXPECT_FALSE(list->load_index(vm, Value::from_int(-4)).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::IndexOutOfBounds);
    EXPECT_EQ(message, "Runtime: IndexOutOfBounds list index -4 out of range");
}

TEST(ObjList, StoreIndexWritesAndChecks) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    list->elements().push(Value::from_int(1));
    EXPECT_TRUE(list->store_index(vm, Value::from_int(0), Value::from_int(9)));
    EXPECT_EQ(list->elements()[0].as_int(), 9);

    // 键检查同读:非整数 TypeMismatch、越界 IndexOutOfBounds,失败不写不动长度(不自动增长)。
    EXPECT_FALSE(list->store_index(vm, Value::from_f64(0.5), Value::from_int(0)));
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::TypeMismatch);
    EXPECT_FALSE(list->store_index(vm, Value::from_int(1), Value::from_int(0)));
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
    EXPECT_EQ(list->elements().size(), 1u);
    EXPECT_EQ(list->elements()[0].as_int(), 9);
}

// 负下标写对称:xs[-1] = v 覆写末槽;越界不写不动长度。
// ---- 切片(Range 键:含否上界/无上界/负端点/空切片/倒序产出/越界 fail-fast/只读) ----

TEST(ObjList, SliceYieldsNewList) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, 1, 3, false);
    guard.push(r1);
    expect_slice(vm, list, r1, {20, 30, 40}); // xs[1..3] 闭区间
    auto r2 = new_range(gc, 1, 3, true);
    guard.push(r2);
    expect_slice(vm, list, r2, {20, 30}); // xs[1...3] 半开
    auto r3 = new_range(gc, 3);
    guard.push(r3);
    expect_slice(vm, list, r3, {40, 50}); // xs[3..] 无上界到末尾
    auto r4 = new_range(gc, 0);
    guard.push(r4);
    expect_slice(vm, list, r4, {10, 20, 30, 40, 50}); // xs[0..] 全表
}

TEST(ObjList, SliceNegativeEndpoints) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, -3);
    guard.push(r1);
    expect_slice(vm, list, r1, {30, 40, 50}); // xs[-3..] 尾三
    auto r2 = new_range(gc, -1);
    guard.push(r2);
    expect_slice(vm, list, r2, {50}); // xs[-1..] 末元素
    auto r3 = new_range(gc, -3, -1, false);
    guard.push(r3);
    expect_slice(vm, list, r3, {30, 40, 50}); // xs[-3..-1]
    // 正起点配负终点:原始端点递减,归一化后仍正序,不得被判成倒序。
    auto r4 = new_range(gc, 1, -1, false);
    guard.push(r4);
    expect_slice(vm, list, r4, {20, 30, 40, 50}); // xs[1..-1] 第二个到最后一个
    auto r5 = new_range(gc, 0, -1, true);
    guard.push(r5);
    expect_slice(vm, list, r5, {10, 20, 30, 40}); // xs[0...-1] 末元素不含
}

TEST(ObjList, SliceEmptyAndSingle) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, 2, 2, true);
    guard.push(r1);
    expect_slice(vm, list, r1, {}); // xs[2...2] 空 range 产空 list
    auto r2 = new_range(gc, 2, 2, false);
    guard.push(r2);
    expect_slice(vm, list, r2, {30}); // xs[2..2] 单元素
}

TEST(ObjList, SliceReversedRangeYieldsReversedOrder) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, 3, 1, false);
    guard.push(r1);
    expect_slice(vm, list, r1, {40, 30, 20}); // xs[3..1] 倒序闭区间
    auto r2 = new_range(gc, 3, 1, true);
    guard.push(r2);
    expect_slice(vm, list, r2, {40, 30}); // xs[3...1] 倒序半开(少走终点 1)
    auto r3 = new_range(gc, 4, 0, false);
    guard.push(r3);
    expect_slice(vm, list, r3, {50, 40, 30, 20, 10}); // 全表倒序
    auto r4 = new_range(gc, -1, -3, false);
    guard.push(r4);
    expect_slice(vm, list, r4, {50, 40, 30}); // 负端点倒序
    // 原始端点递增但从尾计数后越过终点(归一化 from=4 > to=0):同判倒序,产出倒序段。
    auto r5 = new_range(gc, -1, 0, true);
    guard.push(r5);
    expect_slice(vm, list, r5, {50, 40, 30, 20});
    auto r6 = new_range(gc, 2, 2, true);
    guard.push(r6);
    expect_slice(vm, list, r6, {}); // 两端相等不含上界:两方向同一个空切片判据
}

TEST(ObjList, SliceOutOfBoundsFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, 0, 10, false);
    guard.push(r1);
    EXPECT_FALSE(list->load_index(vm, Value::from_obj(r1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
    auto r2 = new_range(gc, -6);
    guard.push(r2);
    EXPECT_FALSE(list->load_index(vm, Value::from_obj(r2)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
}

// 无上界形态 = 后缀语义:起点允许 == 长度(得空段),故空容器、起点恰在末元素之后都给空 list;
// 越过长度仍越界。
TEST(ObjList, SliceUnboundedYieldsSuffixAllowingEmpty) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   r1    = new_range(gc, 5); // xs[5..] 长度 5:起点在末元素之后 -> 空段
    guard.push(r1);
    expect_slice(vm, list, r1, {});
    auto r2 = new_range(gc, -5); // xs[-5..] 首元素起到末尾:全表
    guard.push(r2);
    expect_slice(vm, list, r2, {10, 20, 30, 40, 50});

    auto empty = make_list(gc, guard);
    auto r3    = new_range(gc, 0); // [][0..] 空容器 -> 空段
    guard.push(r3);
    expect_slice(vm, empty, r3, {});
}

TEST(ObjList, SliceOnEmptyListFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    // 有上界形态无端点可取(空容器无实元素位置):越界失败;无上界形态走后缀语义给空段(见上一例)。
    auto r1 = new_range(gc, 2, 2, true);
    guard.push(r1);
    EXPECT_FALSE(list->load_index(vm, Value::from_obj(r1)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
    auto r2 = new_range(gc, 1, 0, false);
    guard.push(r2);
    EXPECT_FALSE(list->load_index(vm, Value::from_obj(r2)).has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
}

TEST(ObjList, StoreSliceFails) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_five(gc, guard);
    auto   range = new_range(gc, 0, 2, false);
    guard.push(range);
    // Range 键写不特殊对待:落整数键检查的统一文案(切片写不设定向错)。
    EXPECT_FALSE(list->store_index(vm, Value::from_obj(range), Value::from_int(9)));
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::TypeMismatch);
    EXPECT_EQ(message, "Runtime: TypeMismatch list index must be an integer, got Range");
}

TEST(ObjList, StoreIndexNegativeWritesFromTail) {
    AriaVM      vm;
    auto&       gc    = vm.gc();
    auto        guard = gc.make_guard();
    auto        list  = make_list(gc, guard);
    const Value src[] = {Value::from_int(1), Value::from_int(2)};
    list->elements().copy_from(src);
    EXPECT_TRUE(list->store_index(vm, Value::from_int(-1), Value::from_int(9)));
    EXPECT_EQ(list->elements()[1].as_int(), 9);
    EXPECT_TRUE(list->store_index(vm, Value::from_int(-2), Value::from_int(8)));
    EXPECT_EQ(list->elements()[0].as_int(), 8);
    EXPECT_FALSE(list->store_index(vm, Value::from_int(-3), Value::from_int(0)));
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::IndexOutOfBounds);
    EXPECT_EQ(list->elements().size(), 2u);
}

// ---- 命名成员协议(load_field → VM 的 List bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根(计划 D1)。
TEST(ObjList, BootstrapClassContract) {
    AriaVM vm;
    auto*  list_class = vm.list_class();
    ASSERT_NE(list_class, nullptr);
    EXPECT_EQ(list_class->name()->view(), "List");
    EXPECT_EQ(list_class->superclass(), vm.object_class());
}

// 命中恒绑定:bound 的 receiver 是本 list、method 是类表内的原生函数(注册名 intern 同指针,
// load_field 传入的 new_string("push") 与注册名命中)。
TEST(ObjList, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   bound = list->load_field(vm, new_string(gc, "push"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(list)));
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "push");
}

// init 沿链解析到 Object 根的 no-op init(List 表自身无 init;调用返回 receiver 自身的语义
// 钉在 Compiler.ListInitResolvesToObjectRootNoOp)。
TEST(ObjList, LoadFieldInitResolvesToObjectRoot) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    auto   bound = list->load_field(vm, new_string(gc, "init"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "init");
}

// miss:类措辞 fail 随协议透传(基类默认拿 receiver debug_repr 当主语的旧文案不复存在)。
TEST(ObjList, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   list  = make_list(gc, guard);
    EXPECT_FALSE(list->load_field(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class List> has no member 'nope'");
}

// stress collect 后类链仍解析:bootstrap 类经寄存器组根、方法原生经类链 field_ 表级联标根。
TEST(ObjList, BootstrapSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       list    = make_list(gc, guard);
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    auto bound = list->load_field(vm, new_string(gc, "pop"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "pop"); // name() 经 bound 的原生取名,存活即链完好
}
