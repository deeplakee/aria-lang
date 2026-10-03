#include <gtest/gtest.h>

#include <cmath>
#include <limits>

#include "util/util.hpp"

using namespace aria;
using util::abs_diff;
using util::parse_float_text;
using util::parse_int_text;
using util::resolve_index;
using util::resolve_position;
using util::take;
using util::UnsignedInteger;

static_assert(!UnsignedInteger<bool>); // bool 走具名重载,不入归零形态
static_assert(UnsignedInteger<usize>);
static_assert(UnsignedInteger<u32>);
static_assert(!UnsignedInteger<i64>);

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

// ---------------------------------------------------------------------------
// parse_int_text：整串十进制整数文本（数据语法，非源码字面量语法）
// ---------------------------------------------------------------------------

TEST(ParseIntText, WholeStringWithOptionalSign) {
    EXPECT_EQ(parse_int_text("42"), 42);
    EXPECT_EQ(parse_int_text("-7"), -7);
    EXPECT_EQ(parse_int_text("+7"), 7); // from_chars 只认 '-'，'+' 由本函数剥
    EXPECT_EQ(parse_int_text("0"), 0);
    EXPECT_EQ(parse_int_text("007"), 7); // 前导零无特殊含义
}

TEST(ParseIntText, RejectsAnythingButWholeDecimalInteger) {
    EXPECT_EQ(parse_int_text(""), std::nullopt);
    EXPECT_EQ(parse_int_text("+"), std::nullopt);
    EXPECT_EQ(parse_int_text("-"), std::nullopt);
    EXPECT_EQ(parse_int_text("abc"), std::nullopt);
    EXPECT_EQ(parse_int_text(" 42"), std::nullopt); // 不跳空白（调用方自行 trim）
    EXPECT_EQ(parse_int_text("42 "), std::nullopt);
    EXPECT_EQ(parse_int_text("4 2"), std::nullopt);
    EXPECT_EQ(parse_int_text("3.7"), std::nullopt); // 小数形只属 parse_float_text
    EXPECT_EQ(parse_int_text("1e3"), std::nullopt);
    EXPECT_EQ(parse_int_text("1_000"), std::nullopt); // 下划线与进制前缀是源码字面量语法
    EXPECT_EQ(parse_int_text("0x10"), std::nullopt);
    EXPECT_EQ(parse_int_text("0b101"), std::nullopt);
}

TEST(ParseIntText, OutOfI48RangeIsNullopt) {
    EXPECT_EQ(parse_int_text("140737488355327"), 140737488355327);   // kIntMax = 2^47 - 1
    EXPECT_EQ(parse_int_text("-140737488355328"), -140737488355328); // kIntMin = -2^47
    EXPECT_EQ(parse_int_text("140737488355328"), std::nullopt);      // 越上界一
    EXPECT_EQ(parse_int_text("-140737488355329"), std::nullopt);     // 越下界一
    EXPECT_EQ(parse_int_text("99999999999999999999"), std::nullopt); // 连 i64 都不收
}

// ---------------------------------------------------------------------------
// parse_float_text：整串十进制浮点文本（含指数形与 inf/nan，与 str(f64) 输出往返一致）
// ---------------------------------------------------------------------------

TEST(ParseFloatText, DecimalForms) {
    EXPECT_EQ(parse_float_text("3.5"), 3.5);
    EXPECT_EQ(parse_float_text("-3.5"), -3.5);
    EXPECT_EQ(parse_float_text("+3.5"), 3.5);
    EXPECT_EQ(parse_float_text("3"), 3.0); // 整数形给浮点值
    EXPECT_EQ(parse_float_text("1e3"), 1000.0);
    EXPECT_EQ(parse_float_text("1E-3"), 0.001);
    EXPECT_EQ(parse_float_text(".5"), 0.5); // 源码文法禁 .5/5.，数据文本收（strtod 口径）
    EXPECT_EQ(parse_float_text("5."), 5.0);
    EXPECT_EQ(parse_float_text("inf"), std::numeric_limits<f64>::infinity());
    const auto nan = parse_float_text("nan"); // NaN 是值不是失败
    ASSERT_TRUE(nan.has_value());
    EXPECT_TRUE(std::isnan(*nan));
}

TEST(ParseFloatText, RejectsAnythingButWholeDecimalFloat) {
    EXPECT_EQ(parse_float_text(""), std::nullopt);
    EXPECT_EQ(parse_float_text("abc"), std::nullopt);
    EXPECT_EQ(parse_float_text("3.5 "), std::nullopt);
    EXPECT_EQ(parse_float_text("."), std::nullopt);
    EXPECT_EQ(parse_float_text("1_000.5"), std::nullopt);
    EXPECT_EQ(parse_float_text("0x10"), std::nullopt);
    EXPECT_EQ(parse_float_text("1e400"), std::nullopt); // 越 f64 域：不饱和成 inf，与解析失败同路
}

TEST(Take, PointerReturnsAndNullsOut) {
    auto*      owned = new int(42);
    int*       p     = owned;
    const int* taken = take(p);
    EXPECT_EQ(taken, owned);
    EXPECT_EQ(p, nullptr);
    delete taken; // const 指针可直接 delete
}

TEST(Take, UnsignedReturnsAndZeroes) {
    usize remaining = 7;
    EXPECT_EQ(take(remaining), static_cast<usize>(7));
    EXPECT_EQ(remaining, static_cast<usize>(0));
    u32 hash = 0xDEADBEEF;
    EXPECT_EQ(take(hash), static_cast<u32>(0xDEADBEEF));
    EXPECT_EQ(hash, static_cast<u32>(0));
}

TEST(Take, BoolReturnsAndSetsFalse) {
    bool dirty = true;
    EXPECT_TRUE(take(dirty));
    EXPECT_FALSE(dirty);
    bool clean = false;
    EXPECT_FALSE(take(clean));
    EXPECT_FALSE(clean);
}

TEST(Take, OptionalOverloadUnaffected) {
    Opt<int> opt{5};
    EXPECT_EQ(take(opt), Opt<int>{5});
    EXPECT_FALSE(opt.has_value());
}
