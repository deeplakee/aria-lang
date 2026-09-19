// ObjMap(值表示/equals/下标协议/load_field 绑定)与 ObjMapIterator(槽位扫描产出 [k,v])的
// 对象层测试;错误白盒取件与 GC 守卫形态同 test_objlist/test_objiterator。
#include <gtest/gtest.h>

#include <string>
#include <tuple>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

using aria::AriaVM;
using aria::ErrorCode;
using aria::GC;
using aria::new_map;
using aria::new_map_iterator;
using aria::new_string;
using aria::ObjBoundMethod;
using aria::ObjException;
using aria::ObjList;
using aria::ObjMap;
using aria::ObjMapIterator;
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

    // 建 map 并入根,返回后调用方自行 set 键值(trivial 分配不触 GC)。
    ObjMap* make_map(GC& gc, GC::Guard& guard) {
        auto map = new_map(gc);
        guard.push(map);
        return map;
    }

    // 建 list 并入根,返回后调用方自行 push 元素(嵌套值构造用)。
    ObjList* make_list(GC& gc, GC::Guard& guard) {
        auto list = new_list(gc);
        guard.push(list);
        return list;
    }

    // 建串并入根(建时 stress collect 在串诞生前完成,set 前无 GC 点)。
    ObjString* make_string(GC& gc, GC::Guard& guard, const StringView src) {
        auto s = new_string(gc, src);
        guard.push(s);
        return s;
    }

    // 建 map 迭代器并入临时根(断言期存活;map 由调用方另守)。
    ObjMapIterator* make_map_iterator(GC& gc, GC::Guard& guard, ObjMap* map) {
        auto iter = new_map_iterator(gc, map);
        guard.push(iter);
        return iter;
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

    // 迭代器 next 产出的 [k, v] pair 拆值(元素恒二元 list;pair list 白色,入临时根再检视)。
    Pair<Value, Value> take_pair(AriaVM& vm, GC::Guard& guard, ObjMapIterator* iter) {
        auto item = iter->next(vm);
        EXPECT_TRUE(item.has_value());
        guard.push(item->as_obj());
        const auto list = try_obj<ObjList>(*item);
        EXPECT_NE(list, nullptr);
        if (list == nullptr || list->elements().size() != 2) {
            return {Value::nil_val(), Value::nil_val()};
        }
        return {list->elements()[0], list->elements()[1]};
    }

} // namespace

// ---- ObjMap 本体 ----

TEST(ObjMap, Basics) {
    GC   gc;
    auto guard = gc.make_guard();
    auto map   = make_map(gc, guard);
    EXPECT_TRUE(aria::Object::is<ObjMap>(map));
    EXPECT_EQ(map->type(), aria::ObjType::MAP);
    EXPECT_EQ(map->type_name(), aria::StringView{"Map"});
    EXPECT_EQ(map->table().size(), 0u);
    EXPECT_EQ(map->debug_repr(), "{}"); // 空 map 字面量形态
}

// 任意键读:命中返回值;键判等按表内语义 ===(int 1 与 f64 1.0 是不同键)。
TEST(ObjMap, LoadIndexArbitraryKeys) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   s     = make_string(gc, guard, "key");
    auto   map   = make_map(gc, guard);
    map->table().set(Value::from_int(1), Value::from_int(10));
    map->table().set(Value::from_obj(s), Value::from_int(20));
    map->table().set(Value::nil_val(), Value::from_int(30));

    auto by_int = map->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(by_int.has_value());
    EXPECT_EQ(by_int->as_int(), 10);
    auto by_str = map->load_index(vm, Value::from_obj(s));
    ASSERT_TRUE(by_str.has_value());
    EXPECT_EQ(by_str->as_int(), 20);
    auto by_nil = map->load_index(vm, Value::nil_val());
    ASSERT_TRUE(by_nil.has_value());
    EXPECT_EQ(by_nil->as_int(), 30);

    // int 1 与 f64 1.0 是不同键(=== 表内语义):互不命中。
    auto as_f64 = map->load_index(vm, Value::from_f64(1.0));
    EXPECT_FALSE(as_f64.has_value());
    EXPECT_EQ(take_pending_error(vm).first, ErrorCode::KeyError);
}

// miss:KeyError,键走 debug 形入文案(int 裸数字、字符串带引号)。
TEST(ObjMap, LoadIndexMissFailsWithKeyInMessage) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    map->table().set(Value::from_int(1), Value::from_int(10));

    EXPECT_FALSE(map->load_index(vm, Value::from_int(99)).has_value());
    auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::KeyError);
    EXPECT_EQ(message, "Runtime: KeyError map key not found: 99");

    const auto nope = make_string(gc, guard, "nope");
    EXPECT_FALSE(map->load_index(vm, Value::from_obj(nope)).has_value());
    std::tie(code, message) = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::KeyError);
    EXPECT_EQ(message, "Runtime: KeyError map key not found: \"nope\"");
}

// 写恒成功:新增键与覆写存量键两条路径均 true;覆写不动 size。
TEST(ObjMap, StoreIndexAlwaysSucceeds) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    EXPECT_TRUE(map->store_index(vm, Value::from_int(1), Value::from_int(10)));
    EXPECT_EQ(map->table().size(), 1u);
    auto read = map->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->as_int(), 10);

    EXPECT_TRUE(map->store_index(vm, Value::from_int(1), Value::from_int(99))); // 覆写
    EXPECT_EQ(map->table().size(), 1u);
    read = map->load_index(vm, Value::from_int(1));
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->as_int(), 99);
}

// equals 按内容:同内容真(插入序无关)、值异/缺键/跨类型假、值按 == 递归(嵌套 list)。
TEST(ObjMap, EqualsIsContentRecursive) {
    GC   gc;
    auto guard = gc.make_guard();
    auto s     = make_string(gc, guard, "v");

    // 同内容不同对象、插入序不同:逐键命中相等。
    auto lhs = make_map(gc, guard);
    lhs->table().set(Value::from_int(1), Value::from_int(10));
    lhs->table().set(Value::from_obj(s), Value::from_int(20));
    auto rhs = make_map(gc, guard);
    rhs->table().set(Value::from_obj(s), Value::from_int(20));
    rhs->table().set(Value::from_int(1), Value::from_int(10));
    EXPECT_TRUE(lhs->equals(rhs));
    EXPECT_TRUE(value_equal(Value::from_obj(lhs), Value::from_obj(rhs)));      // == 走 equals 虚分派
    EXPECT_FALSE(value_identical(Value::from_obj(lhs), Value::from_obj(rhs))); // === 恒指针

    // 值不同 / 缺键。
    auto differing = make_map(gc, guard);
    differing->table().set(Value::from_int(1), Value::from_int(11));
    differing->table().set(Value::from_obj(s), Value::from_int(20));
    EXPECT_FALSE(lhs->equals(differing));
    auto missing = make_map(gc, guard);
    missing->table().set(Value::from_int(1), Value::from_int(10));
    EXPECT_FALSE(lhs->equals(missing));

    // 值按 == 递归:内容相等的嵌套 list 值判真。
    auto list_a = make_list(gc, guard);
    auto list_b = make_list(gc, guard);
    list_a->elements().push(Value::from_int(7));
    list_b->elements().push(Value::from_int(7));
    auto with_a = make_map(gc, guard);
    with_a->table().set(Value::from_int(1), Value::from_obj(list_a));
    auto with_b = make_map(gc, guard);
    with_b->table().set(Value::from_int(1), Value::from_obj(list_b));
    EXPECT_TRUE(with_a->equals(with_b));

    // 跨类型恒 false。
    EXPECT_FALSE(lhs->equals(s));
}

// 键判等 ===:int 1 与 f64 1.0 双键共存,两键均参与 equals 逐键命中。
TEST(ObjMap, EqualsKeysAreIdenticalSemantics) {
    GC   gc;
    auto guard = gc.make_guard();
    auto a     = make_map(gc, guard);
    a->table().set(Value::from_int(1), Value::from_int(10));
    a->table().set(Value::from_f64(1.0), Value::from_int(20));
    EXPECT_EQ(a->table().size(), 2u);

    auto b = make_map(gc, guard);
    b->table().set(Value::from_f64(1.0), Value::from_int(20));
    b->table().set(Value::from_int(1), Value::from_int(10));
    EXPECT_TRUE(a->equals(b));

    b->table().set(Value::from_f64(1.0), Value::from_int(99)); // 改 f64 键的值 -> 逐键命中见差异
    EXPECT_FALSE(a->equals(b));
}

// 调试渲染:单键形态精确(嵌套字符串带引号、嵌套容器递归);多键序随槽位,断言片段。
TEST(ObjMap, DebugRepr) {
    GC   gc;
    auto guard = gc.make_guard();
    auto k     = make_string(gc, guard, "k");
    auto inner = make_list(gc, guard);
    auto ab    = make_string(gc, guard, "ab");
    inner->elements().push(Value::from_int(1));
    inner->elements().push(Value::from_obj(ab));
    auto map = make_map(gc, guard);
    map->table().set(Value::from_obj(k), Value::from_obj(inner));
    EXPECT_EQ(map->debug_repr(), "{\"k\": [1, \"ab\"]}");
    EXPECT_EQ(map->to_string(), "{\"k\": [1, \"ab\"]}");                         // 显示同文案
    EXPECT_EQ(aria::format_value(Value::from_obj(map)), "{\"k\": [1, \"ab\"]}"); // str/print 位

    auto multi = make_map(gc, guard);
    multi->table().set(Value::from_int(1), Value::from_int(10));
    multi->table().set(Value::from_int(2), Value::from_int(20));
    const String repr = multi->debug_repr();
    EXPECT_EQ(repr.front(), '{');
    EXPECT_EQ(repr.back(), '}');
    EXPECT_NE(repr.find("1: 10"), String::npos);
    EXPECT_NE(repr.find("2: 20"), String::npos);
    EXPECT_NE(repr.find(", "), String::npos);
}

// 环防护:自引用在渲染路径上重遇即截断 "{...}"(PrintGuard,防无限递归栈溢出)。
TEST(ObjMap, DebugReprSelfCycleTruncates) {
    GC   gc;
    auto guard = gc.make_guard();
    auto map   = make_map(gc, guard);
    map->table().set(Value::from_int(1), Value::from_obj(map));
    EXPECT_EQ(map->debug_repr(), "{1: {...}}");
    EXPECT_EQ(map->debug_repr(), "{1: {...}}"); // 前次守卫已出栈,再次渲染不受影响
}

// stress GC:map 为唯一根,键值串经 table_.trace 存活;漏标即丢。
TEST(ObjMap, TraceStressKeepsEntries) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc  = vm.gc();
    ObjString* key = nullptr;
    ObjMap*    map = nullptr;
    {
        auto guard       = gc.make_guard();
        key              = make_string(gc, guard, "a long map key string!!!");
        map              = make_map(gc, guard);
        const auto value = make_string(gc, guard, "a long map value string!!!");
        map->table().set(Value::from_obj(key), Value::from_obj(value));
        // 作用域退出:临时根弹出,键值串此后仅经 map.trace 可达
    }
    auto       guard   = gc.make_guard(map);        // 只根 map
    const auto trigger = new_string(gc, "trigger"); // stress collect:键值串经 trace 存活
    guard.push(trigger);
    const usize before = gc.bytes_allocated();
    gc.collect(); // 显式 collect(不分配):若 trace 漏标,失根对象在此掉数
    EXPECT_EQ(gc.bytes_allocated(), before);
    ASSERT_EQ(map->table().size(), 1u);
    auto value = map->load_index(vm, Value::from_obj(key));
    ASSERT_TRUE(value.has_value());
    const auto survived = aria::Object::try_as<ObjString>(value->as_obj());
    ASSERT_NE(survived, nullptr);
    EXPECT_EQ(survived->view(), "a long map value string!!!");
}

// 未根 map 被 sweep(壳 + 表缓冲若无他根一并回收)。
TEST(ObjMap, UnrootedMapSwept) {
    GC gc;
    {
        auto guard = gc.make_guard();
        (void) make_string(gc, guard, "orphan");
        (void) make_map(gc, guard); // 守卫退出后双双失根
    }
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
}

// ---- 命名成员协议(load_field → VM 的 Map bootstrap 类) ----

// bootstrap 契约:类名与 type() 类型名一致、super 挂 Object 根(计划 D1)。
TEST(ObjMap, BootstrapClassContract) {
    AriaVM vm;
    auto*  map_class = vm.map_class();
    ASSERT_NE(map_class, nullptr);
    EXPECT_EQ(map_class->name()->view(), "Map");
    EXPECT_EQ(map_class->superclass(), vm.object_class());
}

// 命中恒绑定:bound 的 receiver 是本 map、method 是类表内的原生函数 iter。
TEST(ObjMap, LoadFieldBindsNativeToReceiver) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    auto   bound = map->load_field(vm, new_string(gc, "iter"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj()); // bound 白色,入临时根再检视
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_TRUE(value_identical(method->receiver(), Value::from_obj(map)));
    const auto native = aria::Object::try_as<ObjNativeFn>(method->method().as_obj());
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->name()->view(), "iter");
}

// miss:类措辞 fail 随协议透传(与实例路径同文案形)。
TEST(ObjMap, LoadFieldMissFailsWithClassWording) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    EXPECT_FALSE(map->load_field(vm, new_string(gc, "nope")).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::UndefinedProperty);
    EXPECT_EQ(message, "Runtime: UndefinedProperty <class Map> has no member 'nope'");
}

// stress collect 后类链仍解析:bootstrap 类经寄存器组根、方法原生经类链 field_ 表级联标根。
TEST(ObjMap, BootstrapSurvivesStressCollect) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&      gc      = vm.gc();
    auto       guard   = gc.make_guard();
    auto       map     = make_map(gc, guard);
    const auto trigger = new_string(gc, "trigger"); // stress:分配即 collect
    guard.push(trigger);
    auto bound = map->load_field(vm, new_string(gc, "iter"));
    ASSERT_TRUE(bound.has_value());
    guard.push(bound->as_obj());
    const auto method = try_obj<ObjBoundMethod>(*bound);
    ASSERT_NE(method, nullptr);
    EXPECT_EQ(method->name()->view(), "iter"); // name() 经 bound 的原生取名,存活即链完好
}

// ---- ObjMapIterator(槽位扫描游标,[k, v] pair 产出) ----

// 游标推进:逐占用槽产出 [k, v] 二元 list,取尽后 has_next 恒 false。
TEST(ObjMapIterator, CursorYieldsKeyValuePairs) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   ka    = make_string(gc, guard, "a");
    auto   kb    = make_string(gc, guard, "b");
    auto   map   = make_map(gc, guard);
    map->table().set(Value::from_obj(ka), Value::from_int(1));
    map->table().set(Value::from_obj(kb), Value::from_int(2));
    auto iter = make_map_iterator(gc, guard, map);

    ASSERT_TRUE(iter->has_next());
    const auto [k0, v0] = take_pair(vm, guard, iter);
    EXPECT_TRUE(value_identical(k0, Value::from_obj(ka)) || value_identical(k0, Value::from_obj(kb)));
    EXPECT_TRUE(v0.as_int() == 1 || v0.as_int() == 2);

    ASSERT_TRUE(iter->has_next());
    const auto [k1, v1] = take_pair(vm, guard, iter);
    EXPECT_TRUE(value_identical(k1, Value::from_obj(ka)) || value_identical(k1, Value::from_obj(kb)));
    EXPECT_TRUE(v1.as_int() == 1 || v1.as_int() == 2);
    EXPECT_FALSE(value_identical(k0, k1)); // 两步产出不同键(游标推进)
    EXPECT_FALSE(iter->has_next());
}

// 空 map:has_next 恒 false;next 越界 fail-fast(IterationExhausted),取尽后 has_next 仍 false。
TEST(ObjMapIterator, NextPastEndFailsFast) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    auto   iter  = make_map_iterator(gc, guard, map);
    EXPECT_FALSE(iter->has_next());
    EXPECT_FALSE(iter->next(vm).has_value());
    const auto [code, message] = take_pending_error(vm);
    EXPECT_EQ(code, ErrorCode::IterationExhausted);
    EXPECT_EQ(message, "Runtime: IterationExhausted iterator exhausted");
    EXPECT_FALSE(iter->has_next());
}

// 删除键后扫描:erase 的槽被游标跳过,余键仍被产出。
TEST(ObjMapIterator, SkipsErasedSlots) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    map->table().set(Value::from_int(1), Value::from_int(10));
    map->table().set(Value::from_int(2), Value::from_int(20));
    map->table().set(Value::from_int(3), Value::from_int(30));
    map->table().erase(Value::from_int(2));
    auto iter = make_map_iterator(gc, guard, map);

    int visited = 0;
    while (iter->has_next()) {
        const auto [k, v] = take_pair(vm, guard, iter);
        EXPECT_NE(k.as_int(), 2); // 已删键不产出
        EXPECT_EQ(k.as_int() * 10, v.as_int());
        ++visited;
    }
    EXPECT_EQ(visited, 2);
}

// 双迭代器游标独立:一个取尽不影响另一个从头取。
TEST(ObjMapIterator, TwoIteratorsCursorsIndependent) {
    AriaVM vm;
    auto&  gc    = vm.gc();
    auto   guard = gc.make_guard();
    auto   map   = make_map(gc, guard);
    map->table().set(Value::from_int(1), Value::from_int(10));
    map->table().set(Value::from_int(2), Value::from_int(20));
    auto first  = make_map_iterator(gc, guard, map);
    auto second = make_map_iterator(gc, guard, map);
    ASSERT_TRUE(first->next(vm).has_value());
    ASSERT_TRUE(first->next(vm).has_value());
    EXPECT_FALSE(first->has_next()); // first 取尽
    EXPECT_TRUE(second->has_next()); // second 不受影响
}

// stress GC:迭代器为唯一根,map 及其键值经 trace 级联存活;next 的 pair 铸造不触 GC 丢对象。
TEST(ObjMapIterator, TraceStressKeepsSource) {
    AriaVM vm;
    vm.gc().set_stress(true);
    auto&           gc   = vm.gc();
    ObjMapIterator* iter = nullptr;
    {
        auto       guard = gc.make_guard();
        auto       map   = make_map(gc, guard);
        const auto key   = make_string(gc, guard, "stress key string");
        const auto value = make_string(gc, guard, "stress value string");
        map->table().set(Value::from_obj(key), Value::from_obj(value));
        iter = make_map_iterator(gc, guard, map);
    }
    auto       guard   = gc.make_guard(iter);       // 只根迭代器:map 仅经 iter->trace 可达
    const auto trigger = new_string(gc, "trigger"); // stress collect
    guard.push(trigger);
    ASSERT_TRUE(iter->has_next());
    const auto [k, v] = take_pair(vm, guard, iter);
    const auto key    = aria::Object::try_as<ObjString>(k.as_obj());
    const auto value  = aria::Object::try_as<ObjString>(v.as_obj());
    ASSERT_NE(key, nullptr);
    ASSERT_NE(value, nullptr);
    EXPECT_EQ(key->view(), "stress key string");
    EXPECT_EQ(value->view(), "stress value string");
}
