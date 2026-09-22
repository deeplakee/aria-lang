#include <gtest/gtest.h>

#include "util/util.hpp"

using namespace aria;
using util::abs_diff;
using util::resolve_index;
using util::resolve_position;

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

// ---------------------------------------------------------------------------
// resolve_position：插入位解析（resolve_index 的姊妹函数，上界放宽到 == size 即追加位）
// ---------------------------------------------------------------------------

TEST(ResolvePosition, PositivePassesThrough) {
    EXPECT_EQ(resolve_position(0, 3), 0u);
    EXPECT_EQ(resolve_position(2, 3), 2u);
    EXPECT_EQ(resolve_position(3, 3), 3u); // == size 即追加位（下标域外多出的合法值）
}

TEST(ResolvePosition, NegativeCountsFromTail) {
    EXPECT_EQ(resolve_position(-1, 3), 2u); // 末元素之前
    EXPECT_EQ(resolve_position(-3, 3), 0u); // 首元素之前（-size 合法边界）
}

TEST(ResolvePosition, OutOfRangeIsNullopt) {
    EXPECT_EQ(resolve_position(4, 3), std::nullopt);
    EXPECT_EQ(resolve_position(-4, 3), std::nullopt); // < -size（追加位无负拼写）
    EXPECT_EQ(resolve_position(1, 0), std::nullopt);
    EXPECT_EQ(resolve_position(0, 0), 0u); // 空表唯一合法位
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
