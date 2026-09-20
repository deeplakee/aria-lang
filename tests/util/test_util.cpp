#include <gtest/gtest.h>

#include "util/util.hpp"

using namespace aria;
using util::abs_diff;
using util::resolve_index;

// ---------------------------------------------------------------------------
// resolve_index：负下标从尾计数 + 越界判定（list/string 下标与切片端点共用）
// ---------------------------------------------------------------------------

TEST(ResolveIndex, PositivePassesThrough) {
    EXPECT_EQ(resolve_index(0, 3), 0u);
    EXPECT_EQ(resolve_index(2, 3), 2u);
}

TEST(ResolveIndex, NegativeCountsFromTail) {
    EXPECT_EQ(resolve_index(-1, 3), 2u); // 末元素
    EXPECT_EQ(resolve_index(-3, 3), 0u); // 首元素（-size 合法边界）
}

TEST(ResolveIndex, OutOfRangeIsNullopt) {
    EXPECT_EQ(resolve_index(3, 3), std::nullopt); // == size
    EXPECT_EQ(resolve_index(100, 3), std::nullopt);
    EXPECT_EQ(resolve_index(-4, 3), std::nullopt); // < -size
    EXPECT_EQ(resolve_index(0, 0), std::nullopt);  // 空容器任何键都越界
    EXPECT_EQ(resolve_index(-1, 0), std::nullopt);
}

TEST(ResolveIndex, OptionalEndpointUnboundedTakesLast) {
    // range 终点语义：nullopt = 无上界 -> 末元素；空容器无末元素同为 nullopt。
    EXPECT_EQ(resolve_index(Opt<i64>{}, 5), 4u);
    EXPECT_EQ(resolve_index(Opt<i64>{}, 1), 0u);
    EXPECT_EQ(resolve_index(Opt<i64>{}, 0), std::nullopt);
}

TEST(ResolveIndex, OptionalEndpointWithValueDelegates) {
    EXPECT_EQ(resolve_index(std::optional<i64>{-1}, 4), 3u);
    EXPECT_EQ(resolve_index(std::optional<i64>{-5}, 4), std::nullopt);
    EXPECT_EQ(resolve_index(std::optional<i64>{2}, 4), 2u);
}

// ---------------------------------------------------------------------------
// abs_diff：两下标距离（取大减小，无符号域不下溢）
// ---------------------------------------------------------------------------

TEST(AbsDiff, LargerMinusSmaller) {
    EXPECT_EQ(abs_diff(0u, 0u), 0u);
    EXPECT_EQ(abs_diff(3u, 7u), 4u);
    EXPECT_EQ(abs_diff(7u, 3u), 4u);
    EXPECT_EQ(abs_diff(0u, 5u), 5u);
}
