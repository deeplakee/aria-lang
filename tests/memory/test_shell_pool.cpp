#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

#include "memory/ShellPool.hpp"

using aria::ShellPool;
using aria::u64;
using aria::u8;
using aria::usize;

namespace {

    // 槽类探针类型:u64 数组保证 alignof = 8,数组长度直定 sizeof(即槽尺寸)。
    struct Slot24 {
        u64 v[3];
    }; // 24 B
    struct Slot40 {
        u64 v[5];
    }; // 40 B(BoundMethod 一档)
    struct Slot48 {
        u64 v[6];
    }; // 48 B
    struct Slot56 {
        u64 v[7];
    }; // 56 B(List/Range 一档)
    struct Edge256 {
        u64 v[32];
    }; // 256 B = 池上限,仍池化
    struct Over256 {
        u64 v[33];
    }; // 264 B,超上限走后端直连

    // 非对齐尾:41 B 上取整到 48,与 Slot48 同格。
    struct Odd41 {
        u64 v[5];
        u8  tail;
    };

} // namespace

TEST(ShellPool, SameClassReusesPushedShell) {
    ShellPool pool;
    void*     first = pool.alloc<Slot40>();
    ASSERT_NE(first, nullptr);
    pool.push(sizeof(Slot40), first);
    EXPECT_EQ(pool.alloc<Slot40>(), first);
}

TEST(ShellPool, CrossClassIsolation) {
    ShellPool pool;
    void*     small = pool.alloc<Slot40>();
    pool.push(sizeof(Slot40), small);
    EXPECT_NE(pool.alloc<Slot56>(), small);
    EXPECT_EQ(pool.alloc<Slot40>(), small);
}

TEST(ShellPool, SameSlotSharesAcrossRoundedSizes) {
    ShellPool pool;
    void*     shell = pool.alloc<Odd41>();
    pool.push(sizeof(Odd41), shell); // 41 上取整落 48 格
    EXPECT_EQ(pool.alloc<Slot48>(), shell);
}

TEST(ShellPool, EdgeSizeStillPooled) {
    ShellPool pool;
    void*     shell = pool.alloc<Edge256>();
    ASSERT_NE(shell, nullptr);
    pool.push(sizeof(Edge256), shell);
    EXPECT_EQ(pool.alloc<Edge256>(), shell);
}

TEST(ShellPool, SpanExhaustionAcquiresNext) {
    ShellPool pool;
    // 2000 > (64 KB - 24 B 头) / 40 B = 1637 槽:越过第一支 span 强制续取
    constexpr usize   kAllocs = 2000;
    aria::List<void*> shells;
    for (usize i = 0; i < kAllocs; ++i) {
        void* shell = pool.alloc<Slot40>();
        ASSERT_NE(shell, nullptr);
        shells.push_back(shell);
    }
    std::sort(shells.begin(), shells.end());
    EXPECT_EQ(std::adjacent_find(shells.begin(), shells.end()), shells.end());
    // 多 span 状态下空壳链照常服务
    pool.push(sizeof(Slot40), shells.back());
    EXPECT_EQ(pool.alloc<Slot40>(), shells.back());
}

TEST(ShellPool, SlotsAreIndependentAndAligned) {
    ShellPool pool;
    auto*     a = static_cast<u8*>(pool.alloc<Slot56>());
    auto*     b = static_cast<u8*>(pool.alloc<Slot56>());
    ASSERT_NE(a, b);
    EXPECT_EQ(reinterpret_cast<usize>(a) % 8, 0);
    EXPECT_EQ(reinterpret_cast<usize>(b) % 8, 0);
    std::memset(a, 0xAA, sizeof(Slot56));
    std::memset(b, 0x55, sizeof(Slot56));
    EXPECT_EQ(a[0], 0xAA);
    EXPECT_EQ(b[0], 0x55);
}

TEST(ShellPool, OversizedFallsBackToBackend) {
    ShellPool pool;
    void*     shell = pool.alloc<Over256>();
    ASSERT_NE(shell, nullptr);
    std::memset(shell, 0x77, sizeof(Over256));
    pool.push(sizeof(Over256), shell); // 后端直连归还,不进池
    // 超大壳往返不影响池化格
    void* pooled = pool.alloc<Slot40>();
    pool.push(sizeof(Slot40), pooled);
    EXPECT_EQ(pool.alloc<Slot40>(), pooled);
}

TEST(ShellPool, DrainOnDestruction) {
    aria::List<void*> shells;
    {
        ShellPool pool;
        for (usize i = 0; i < 3000; ++i) { // 3000 x 24 B = 72 KB:至少两支 span
            shells.push_back(pool.alloc<Slot24>());
        }
        pool.push(sizeof(Slot24), shells.front()); // 空壳链非空状态下析构
    } // ~ShellPool 排空全部 span,含仍被空壳链引用的块
    ShellPool after;
    void*     shell = after.alloc<Slot56>();
    after.push(sizeof(Slot56), shell);
    EXPECT_EQ(after.alloc<Slot56>(), shell);
}
