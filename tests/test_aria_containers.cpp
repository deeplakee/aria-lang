#include <gtest/gtest.h>

#include <format>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"
#include "value/AriaArray.hpp"
#include "value/AriaHashTable.hpp"

using aria::AriaArray;
using aria::AriaHashTable;
using aria::GC;
using aria::new_string;
using aria::String;
using aria::usize;
using aria::Value;

// AriaArray / AriaHashTable 是 C++ 类(非 Object),GC 不自动 trace 它们;其 trace() 由
// 未来的 owner(ObjList/ObjMap,Phase 3)在 collect 的 trace 阶段调用。故此处手动调 trace
// 模拟 owner,验证 trace 正确标记元素 -> 元素存活。

TEST(AriaArray, PushIndexSize) {
    GC        gc;
    AriaArray arr{&gc};
    EXPECT_TRUE(arr.empty());
    arr.push(Value::nil_val());
    arr.push(Value::from_i32(7));
    arr.push(Value::from_f64(3.14));
    EXPECT_EQ(arr.size(), 3u);
    EXPECT_TRUE(arr[0].is_nil());
    EXPECT_EQ(arr[1].as_int(), 7);
    EXPECT_EQ(arr[2].as_f64(), 3.14);
}

TEST(AriaArray, TraceMarksElements) {
    GC        gc;
    AriaArray arr{&gc};
    auto     a = new_string(gc, "long element string a!!!!");
    auto     b = new_string(gc, "long element string b!!!!");
    arr.push(Value::from_obj(a));
    arr.push(Value::from_obj(b));
    arr.push(Value::nil_val()); // 非对象元素:trace 应跳过(nil/int/f64 无对象子节点)
    arr.push(Value::from_i32(42));
    arr.trace(gc); // 标记 a, b(模拟 owner ObjList 调用)
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before); // a, b 被标记 -> 存活
    EXPECT_EQ(a->view(), "long element string a!!!!");
    EXPECT_EQ(b->view(), "long element string b!!!!");
    EXPECT_EQ(arr.size(), 4u);
}

TEST(AriaArray, UntracedElementsCollected) {
    GC        gc;
    AriaArray arr{&gc};
    auto     a = new_string(gc, "long element string a!!!!");
    arr.push(Value::from_obj(a));
    // 不 trace:arr 非 root,a 无根 -> collect 回收
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
    // a 已释放;arr 仍持悬垂 Value,但下面不再解引用它(arr.clear 只截断 len)
    arr.clear();
    EXPECT_TRUE(arr.empty());
}

TEST(AriaHashTable, UpsertFindErase) {
    GC            gc;
    AriaHashTable ht{&gc};
    auto         ka = new_string(gc, "key-a");
    auto         va = new_string(gc, "value-a long enough!!!");
    auto         e  = ht.upsert(Value::from_obj(ka));
    ASSERT_NE(e, nullptr);
    e->value = Value::from_obj(va);
    EXPECT_EQ(ht.size(), 1u);

    auto found = ht.find(Value::from_obj(ka));
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->key.as_obj(), ka);
    EXPECT_EQ(found->value.as_obj(), va);

    // upsert 已有键:返回同 entry,value 保留(未重置为 V{})
    auto e2 = ht.upsert(Value::from_obj(ka));
    EXPECT_EQ(e2, e);
    EXPECT_EQ(e2->value.as_obj(), va);
    EXPECT_EQ(ht.size(), 1u);

    EXPECT_TRUE(ht.erase(Value::from_obj(ka)));
    EXPECT_EQ(ht.find(Value::from_obj(ka)), nullptr);
    EXPECT_EQ(ht.size(), 0u);
    EXPECT_FALSE(ht.erase(Value::from_obj(ka)));
}

TEST(AriaHashTable, MixedValueTypesAsKeys) {
    GC            gc;
    AriaHashTable ht{&gc};
    ht.upsert(Value::nil_val())->value       = Value::from_i32(1);
    ht.upsert(Value::from_bool(true))->value = Value::from_i32(2);
    ht.upsert(Value::from_i32(100))->value   = Value::from_i32(3);
    ht.upsert(Value::from_f64(2.5))->value   = Value::from_i32(4);
    EXPECT_EQ(ht.size(), 4u);
    EXPECT_EQ(ht.find(Value::nil_val())->value.as_int(), 1);
    EXPECT_EQ(ht.find(Value::from_bool(true))->value.as_int(), 2);
    EXPECT_EQ(ht.find(Value::from_i32(100))->value.as_int(), 3);
    EXPECT_EQ(ht.find(Value::from_f64(2.5))->value.as_int(), 4);
    // int 1 与 f64 1.0 是不同键(哈希键用 === value_identical:类型严格)
    EXPECT_EQ(ht.find(Value::from_f64(1.0)), nullptr);
}

TEST(AriaHashTable, ManyEntriesRehash) {
    GC            gc;
    auto          lock = gc.make_lock(); // 禁用 GC:本测关注 HashTable rehash,不测 GC 交互
    AriaHashTable ht{&gc};               // (60 个 intern 串 + ht 分配会超 next_gc_,触发回收未根化的串)
    for (int i = 0; i < 30; ++i) {
        auto k                              = new_string(gc, std::format("key-{}", i));
        auto v                              = new_string(gc, std::format("value-{}", i));
        ht.upsert(Value::from_obj(k))->value = Value::from_obj(v);
    }
    EXPECT_EQ(ht.size(), 30u);
    for (int i = 0; i < 30; ++i) {
        auto k = new_string(gc, std::format("key-{}", i)); // interned:同插入时的指针
        auto e = ht.find(Value::from_obj(k));
        ASSERT_NE(e, nullptr);
        auto v = new_string(gc, std::format("value-{}", i)); // interned:同插入值指针
        EXPECT_EQ(e->value.as_obj(), v);
    }
}

TEST(AriaHashTable, TraceMarksKeysAndValues) {
    GC            gc;
    AriaHashTable ht{&gc};
    auto         k                      = new_string(gc, "trace-key long enough!!!");
    auto         v                      = new_string(gc, "trace-value long enough!");
    ht.upsert(Value::from_obj(k))->value = Value::from_obj(v);
    ht.trace(gc); // 标记 k, v(模拟 owner ObjMap 调用)
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_EQ(gc.bytes_allocated(), before); // k, v 被标记 -> 存活
    EXPECT_EQ(ht.find(Value::from_obj(k))->value.as_obj(), v);
}

TEST(AriaHashTable, UntracedEntriesCollected) {
    GC            gc;
    AriaHashTable ht{&gc};
    auto         k                      = new_string(gc, "untraced-key long enough");
    auto         v                      = new_string(gc, "untraced-value long enuf");
    ht.upsert(Value::from_obj(k))->value = Value::from_obj(v);
    // 不 trace:ht 非 root,k/v 无根 -> collect 回收
    const usize before = gc.bytes_allocated();
    gc.collect();
    EXPECT_LT(gc.bytes_allocated(), before);
    // k, v 已释放;ht 仍持悬垂 Value,但 ~HashTable 只释放 ctrl_/entries_ 数组(不读 Value)
    ht.clear();
    EXPECT_TRUE(ht.empty());
}
