#include <gtest/gtest.h>

#include "memory/GC.hpp"        // 显式 include:本测用 GC 作分配器(默认 Alloc=GC)
#include "memory/HashTable.hpp" // 分配器解耦,不传递 GC.hpp(见 Allocator.hpp)

using aria::GC;
using aria::HashTable;
using aria::u32;
using aria::usize;

namespace {

    // 良好分布的整数哈希(Knuth 乘法),h2 随键变化 -> 练到 h2 过滤路径。
    struct IntHash {
        u32 operator()(int k) const noexcept { return static_cast<u32>(k) * 0x9E3779B9u; }
    };

    struct IntEq {
        bool operator()(int a, int b) const noexcept { return a == b; }
    };

    // 所有键 hash = k*1024 -> h1 = k*8 -> slot = h1 & 7 = 0,全部从槽 0 出发,
    // 强制三角探测;且 h2 = 0 恒定 -> 每个占用槽都比全键,练到探测链全键比较路径。
    struct CollidingIntHash {
        u32 operator()(int k) const noexcept { return static_cast<u32>(k) * 1024u; }
    };

    using IntTable = HashTable<int, int, IntHash, IntEq>;

} // namespace

TEST(HashTable, SetInsertAndFind) {
    GC       gc;
    IntTable ht{&gc};
    EXPECT_TRUE(ht.empty());
    for (int i = 0; i < 20; ++i) {
        ht.set(i, i * 10);
    }
    EXPECT_EQ(ht.size(), 20u);
    for (int i = 0; i < 20; ++i) {
        auto e = ht.find(i);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->key, i);
        EXPECT_EQ(e->value, i * 10);
    }
    EXPECT_EQ(ht.find(999), nullptr);
}

TEST(HashTable, SetExistingOverwrites) {
    GC       gc;
    IntTable ht{&gc};
    ht.set(5, 50);
    ht.set(5, 999); // 已存在:原槽覆写
    auto e = ht.find(5);
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(e->value, 999);
    EXPECT_EQ(ht.size(), 1u); // 未新增
}

TEST(HashTable, EraseMakesNotFound) {
    GC       gc;
    IntTable ht{&gc};
    for (int i = 0; i < 10; ++i) {
        ht.set(i, i);
    }
    EXPECT_EQ(ht.size(), 10u);
    EXPECT_TRUE(ht.erase(5));
    EXPECT_EQ(ht.size(), 9u);
    EXPECT_EQ(ht.find(5), nullptr);
    EXPECT_FALSE(ht.erase(5));   // 已删
    EXPECT_FALSE(ht.erase(999)); // 不存在
    for (int i = 0; i < 10; ++i) {
        if (i == 5) {
            continue;
        }
        EXPECT_NE(ht.find(i), nullptr);
    }
}

TEST(HashTable, EraseAndReinsert) {
    GC       gc;
    IntTable ht{&gc};
    for (int i = 0; i < 6; ++i) {
        ht.set(i, i);
    }
    ht.erase(2);
    ht.erase(4);
    EXPECT_EQ(ht.size(), 4u);
    ht.set(2, 200); // 重新插入被删的键(复用墓碑)
    ht.set(4, 400);
    EXPECT_EQ(ht.find(2)->value, 200);
    EXPECT_EQ(ht.find(4)->value, 400);
    EXPECT_EQ(ht.size(), 6u);
}

TEST(HashTable, RehashGrowsCapacity) {
    GC       gc;
    IntTable ht{&gc};
    EXPECT_EQ(ht.capacity(), 0u);
    ht.set(0, 0);
    EXPECT_EQ(ht.capacity(), 8u); // kInitialCap
    for (int i = 1; i < 7; ++i) {
        ht.set(i, i); // count=7,projected=7 未超 7/8*8=7,不扩容
    }
    EXPECT_EQ(ht.capacity(), 8u);
    EXPECT_EQ(ht.size(), 7u);
    ht.set(7, 7); // projected=8>7 -> 扩容到 16
    EXPECT_EQ(ht.capacity(), 16u);
    EXPECT_EQ(ht.size(), 8u);
    for (int i = 0; i < 8; ++i) {
        EXPECT_NE(ht.find(i), nullptr); // 扩容后所有键仍可查
    }
}

TEST(HashTable, TombstoneCompact) {
    GC       gc;
    IntTable ht{&gc};
    for (int i = 0; i < 3; ++i) {
        ht.set(i, i); // cap=8,count=3
    }
    ht.erase(0); // tomb=1,count=2
    ht.erase(1); // tomb=2,count=1
    EXPECT_EQ(ht.size(), 1u);
    EXPECT_EQ(ht.capacity(), 8u);
    // tomb=2 > cap/8=1,projected=1+2+1=4 <= 7 -> 下次插入原容 compact(清墓碑)
    ht.set(100, 100);
    EXPECT_EQ(ht.capacity(), 8u); // 原容 compact,未扩容
    EXPECT_EQ(ht.find(2)->value, 2);
    EXPECT_EQ(ht.find(100)->value, 100);
    EXPECT_EQ(ht.find(0), nullptr); // 已删
    EXPECT_EQ(ht.find(1), nullptr);
}

TEST(HashTable, CollisionsTriangularProbing) {
    GC                                           gc;
    HashTable<int, int, CollidingIntHash, IntEq> ht{&gc};
    for (int i = 0; i < 6; ++i) {
        ht.set(i, i * 100); // cap=8 容纳 6(<7)
    }
    for (int i = 0; i < 6; ++i) {
        auto e = ht.find(i);
        ASSERT_NE(e, nullptr);
        EXPECT_EQ(e->value, i * 100);
    }
    // 删除中间一个,其余仍可查(探测链跨墓碑)
    EXPECT_TRUE(ht.erase(3));
    for (int i = 0; i < 6; ++i) {
        if (i == 3) {
            EXPECT_EQ(ht.find(i), nullptr);
            continue;
        }
        EXPECT_NE(ht.find(i), nullptr);
    }
}

TEST(HashTable, EmptyTableOps) {
    GC       gc;
    IntTable ht{&gc};
    EXPECT_EQ(ht.find(0), nullptr);
    EXPECT_FALSE(ht.erase(0));
    EXPECT_TRUE(ht.empty());
    ht.clear(); // 不崩
}

TEST(HashTable, Clear) {
    GC       gc;
    IntTable ht{&gc};
    for (int i = 0; i < 10; ++i) {
        ht.set(i, i);
    }
    ht.clear();
    EXPECT_TRUE(ht.empty());
    EXPECT_EQ(ht.find(0), nullptr);
    ht.set(42, 420); // 清空后可重新使用
    EXPECT_EQ(ht.find(42)->value, 420);
    EXPECT_EQ(ht.size(), 1u);
}

TEST(HashTable, DtorReleasesMemory) {
    GC          gc;
    const usize before = gc.bytes_allocated();
    {
        IntTable ht{&gc};
        for (int i = 0; i < 100; ++i) {
            ht.set(i, i); // 多次扩容
        }
        EXPECT_GT(gc.bytes_allocated(), before); // 分配了 ctrl_+entries_
    } // dtor 释放
    EXPECT_EQ(gc.bytes_allocated(), before); // 全部归还
}

// ---- 槽位迭代器(begin/end,跳空槽/墓碑;ObjMap 迭代游标与 trace 的扫描缝) ----

TEST(HashTable, IteratorScansOccupiedSlots) {
    GC       gc;
    IntTable ht{&gc};
    EXPECT_TRUE(ht.begin() == ht.end()); // 空表:无占用槽,begin 即 end

    ht.set(1, 10); // 单元素:全程恰一次解引用
    auto it = ht.begin();
    ASSERT_TRUE(it != ht.end());
    EXPECT_EQ(it->key, 1);
    EXPECT_EQ(it->value, 10);
    ++it;
    EXPECT_TRUE(it == ht.end());

    // 多元素:槽位序逐个命中,erase 的槽被跳过(墓碑非占用)。
    ht.set(2, 20);
    ht.set(3, 30);
    ht.erase(2);
    int visited = 0;
    for (const auto& entry: ht) {
        EXPECT_EQ(entry.key * 10, entry.value);
        ++visited;
    }
    EXPECT_EQ(visited, 2); // 3 - 1 删除
}

TEST(HashTable, IteratorStructuredBindingReadsKeyAndValue) {
    GC       gc;
    IntTable ht{&gc};
    ht.set(7, 70);
    ht.set(9, 90);
    int sum_keys = 0, sum_vals = 0;
    for (const auto& [key, value]: ht) {
        sum_keys += key;
        sum_vals += value;
    }
    EXPECT_EQ(sum_keys, 16);
    EXPECT_EQ(sum_vals, 160);
}
