#include <gtest/gtest.h>

#include "memory/AstArena.hpp"

using aria::AstArena;
using aria::i64;
using aria::List;
using aria::Span;
using aria::StringView;
using aria::u32;
using aria::usize;

namespace {
    // 探针节点/元素：平凡聚合，验证分配、对齐与存活性（AstArena 对元素形态的唯一契约是
    // make 的构造语义与 make_list 的平凡可析构）。
    struct ProbeNode {
        i64        value = 0;
        StringView name{};
    };

    struct ValueEntry {
        i64 a = 0;
        i64 b = 0;
    };
} // namespace

TEST(AstArena, MakeConstructsAndKeepsValues) {
    AstArena arena;
    for (int i = 0; i < 100; ++i) {
        auto* node = arena.make<ProbeNode>(static_cast<i64>(i), StringView{"n"});
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->value, i);
        EXPECT_EQ(node->name, StringView{"n"});
    }
    EXPECT_EQ(arena.node_count(), 100u);
}

TEST(AstArena, AllocationsAreDistinctAndAligned) {
    AstArena   arena;
    ProbeNode* prev = nullptr;
    for (int i = 0; i < 1000; ++i) {
        auto* node = arena.make<ProbeNode>(static_cast<i64>(i), StringView{});
        ASSERT_NE(node, nullptr);
        EXPECT_NE(node, prev);
        EXPECT_EQ(reinterpret_cast<usize>(node) % alignof(ProbeNode), 0u);
        EXPECT_EQ(node->value, i); // 前序节点不被后继分配破坏
        prev = node;
    }
}

TEST(AstArena, GrowsAcrossBlocks) {
    // 首块给到放不下几个节点，逼出块增长；全部元素须存活且互不重叠。
    AstArena arena{64};
    for (int i = 0; i < 5000; ++i) {
        auto* node = arena.make<ProbeNode>(static_cast<i64>(i), StringView{});
        ASSERT_NE(node, nullptr);
        EXPECT_EQ(node->value, i);
    }
    EXPECT_EQ(arena.node_count(), 5000u);
}

TEST(AstArena, MakeListMovesElements) {
    AstArena         arena;
    List<ValueEntry> source;
    source.push_back(ValueEntry{.a = 1, .b = 2});
    source.push_back(ValueEntry{.a = 3, .b = 4});

    const Span<ValueEntry> list = arena.make_list(std::move(source));
    ASSERT_NE(list.data(), nullptr);
    EXPECT_EQ(list.size(), 2u);
    EXPECT_EQ(list[0].a, 1);
    EXPECT_EQ(list[0].b, 2);
    EXPECT_EQ(list[1].a, 3);
    EXPECT_EQ(list[1].b, 4);
    EXPECT_EQ(list.back().a, 3);
    // 遍历口与容器同形。
    i64 sum = 0;
    for (const auto& entry: list) {
        sum += entry.a + entry.b;
    }
    EXPECT_EQ(sum, 10);
}

TEST(AstArena, MakeListEmptyIsZeroAllocation) {
    AstArena         arena;
    List<ValueEntry> source;
    const auto       list = arena.make_list(std::move(source));
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), 0u);
    EXPECT_EQ(list.data(), nullptr);
}

TEST(AstArena, LargeListCopiesAcrossBlockGrowth) {
    // 大列表强制新块整块容纳：元素须连续完整。
    AstArena         arena{64};
    List<ValueEntry> source;
    for (int i = 0; i < 1000; ++i) {
        source.push_back(ValueEntry{.a = i, .b = -i});
    }
    const Span<ValueEntry> list = arena.make_list(std::move(source));
    EXPECT_EQ(list.size(), 1000u);
    for (u32 i = 0; i < list.size(); ++i) {
        ASSERT_EQ(list[i].a, static_cast<i64>(i));
        ASSERT_EQ(list[i].b, -static_cast<i64>(i));
    }
}

TEST(AstArena, AccountingReflectsBlocks) {
    AstArena arena;
    EXPECT_EQ(arena.node_count(), 0u);
    EXPECT_GE(arena.allocated_bytes(), aria::kAstArenaFirstBlockBytes);
    std::ignore = arena.make<ProbeNode>(1, StringView{});
    EXPECT_EQ(arena.node_count(), 1u);
}
