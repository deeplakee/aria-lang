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

TEST(HashTable, ForEachOccupied) {
    GC       gc;
    IntTable ht{&gc};
    for (int i = 0; i < 10; ++i) {
        ht.set(i, i);
    }
    ht.erase(3);
    ht.erase(7);
    int sum_keys = 0, sum_vals = 0, count = 0;
    ht.for_each_occupied([&](const int& k, const int& v) {
        sum_keys += k;
        sum_vals += v;
        ++count;
    });
    EXPECT_EQ(count, 8);     // 10 - 2 删除
    EXPECT_EQ(sum_keys, 35); // 0..9 去掉 3,7 = 45-10
    EXPECT_EQ(sum_vals, 35); // 值同键
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
