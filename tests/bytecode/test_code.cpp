#include <gtest/gtest.h>

#include "bytecode/code.hpp"

using aria::op_symbol;
using aria::OpCode;

// op_symbol:二元算术/比较指令 -> 源码算子记号(VM 数值二元的类型守卫报错消息取此号)。
TEST(OpCode, SymbolForBinaryOperators) {
    EXPECT_EQ(op_symbol(OpCode::ADD), "+");
    EXPECT_EQ(op_symbol(OpCode::SUBTRACT), "-");
    EXPECT_EQ(op_symbol(OpCode::MULTIPLY), "*");
    EXPECT_EQ(op_symbol(OpCode::DIVIDE), "/");
    EXPECT_EQ(op_symbol(OpCode::MOD), "%");
    EXPECT_EQ(op_symbol(OpCode::GREATER), ">");
    EXPECT_EQ(op_symbol(OpCode::GREATER_EQUAL), ">=");
    EXPECT_EQ(op_symbol(OpCode::LESS), "<");
    EXPECT_EQ(op_symbol(OpCode::LESS_EQUAL), "<=");
}

// 非算子指令不在记号表内:取到即 "?"(可见记号暴露编程错误,不静默返空串)。
TEST(OpCode, SymbolForNonOperatorIsPlaceholder) {
    EXPECT_EQ(op_symbol(OpCode::HALT), "?");
    EXPECT_EQ(op_symbol(OpCode::NEGATE), "?");
    EXPECT_EQ(op_symbol(OpCode::STRICT_EQUAL), "?");
}
