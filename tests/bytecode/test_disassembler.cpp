#include <gtest/gtest.h>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/Disassembler.hpp"
#include "bytecode/code.hpp"
#include "value/Value.hpp"

using aria::CodeUnit;
using aria::Disassembler;
using aria::GC;
using aria::kOpCodeCount;
using aria::kOpCodeFormats;
using aria::kOpCodeNames;
using aria::OpCode;
using aria::OpFormat;
using aria::String;
using aria::u16;
using aria::u32;
using aria::u8;
using aria::usize;
using aria::Value;

// 手搓 CodeUnit(emit_op + 裸操作数)驱动反汇编,断言 disassembleInstruction 的单行指令文本
// (与 disassemble() 每行的指令段一致:opcode 名左对齐 16 列 + 两空格 + 操作数段)。
// 用意:锁住表驱动解码的渲染回归 -- 新增 opcode 时若 X 表格式列填错,此处先红。
// 覆盖 OpFormat 十个分发分支里的九个;RegU8(LOAD_REG)分支未在此单测覆盖。

TEST(Disassembler, OpCodeTablesConsistentWithList) {
    // 名字/格式表与 X 表同源生成:数组以 kOpCodeCount 显式定界,行数不符即编译错;此处锁布局哨兵。
    EXPECT_EQ(kOpCodeCount, 63u); // X 表行数哨兵,改指令集须同步改此值
    EXPECT_EQ(kOpCodeNames[0], "HALT");
    EXPECT_EQ(kOpCodeNames[kOpCodeCount - 1], "RETURN");
    EXPECT_EQ(kOpCodeFormats[0], OpFormat::Simple);
}

TEST(Disassembler, SimpleFormat) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::ADD, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "ADD");
}

TEST(Disassembler, U8Format) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::LOAD_LOCAL, 1);
    cu.emit_byte(0x05, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "LOAD_LOCAL        05");
}

TEST(Disassembler, U16Format) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::MAKE_LIST, 1);
    cu.emit_word(0x0102, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "MAKE_LIST         0102");
}

TEST(Disassembler, ConstU16Format) {
    GC        gc;
    CodeUnit  cu{&gc};
    const u16 idx = cu.add_constant(Value::from_i32(42));
    cu.emit_op(OpCode::LOAD_CONST, 1);
    cu.emit_word(idx, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "LOAD_CONST        0000  ; 42");
}

TEST(Disassembler, ImmI8Format) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::LOAD_IMM, 1);
    cu.emit_byte(0xFE, 1); // 位型 i8 = -2
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "LOAD_IMM          FE  ; -2");
}

TEST(Disassembler, JumpFwdFormat) {
    GC        gc;
    CodeUnit  cu{&gc};
    const u32 src = cu.emit_jump(OpCode::JUMP_TRUE, 1); // 占位 off=0
    // 填 2 字节再回填: base=3, target=5 -> off=2
    cu.emit_byte(0xAA, 1);
    cu.emit_byte(0xBB, 1);
    ASSERT_TRUE(cu.patch_jump(src));
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "JUMP_TRUE         0002 -> 0005");
}

TEST(Disassembler, JumpBackFormat) {
    GC       gc;
    CodeUnit cu{&gc};
    ASSERT_TRUE(cu.emit_jump_back(0x0000, 1)); // 跳回 code 起点: base=3, off=3
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "JUMP_BACK         0003 <- 0000");
}

TEST(Disassembler, RangeFlagsFormat) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::MAKE_RANGE, 1);
    cu.emit_byte(0x01, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "MAKE_RANGE        01  ; flags=0x01");
}

TEST(Disassembler, ImportFormat) {
    GC        gc;
    CodeUnit  cu{&gc};
    const u16 idx = cu.add_constant(Value::from_i32(42)); // path 索引本应指 ObjString,此处借 i32 值锁渲染形态
    cu.emit_op(OpCode::IMPORT, 1);
    cu.emit_word(idx, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "IMPORT            0000  ; 42");
}

TEST(Disassembler, PrepareMethodFormat) {
    GC        gc;
    CodeUnit  cu{&gc};
    const u16 idx = cu.add_constant(Value::from_i32(42));
    cu.emit_op(OpCode::PREPARE_METHOD, 1);
    cu.emit_word(idx, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "PREPARE_METHOD    0000  ; 42");
}

TEST(Disassembler, CallMethodFormat) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::CALL_METHOD, 1);
    cu.emit_byte(0x02, 1);
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "CALL_METHOD       02");
}

TEST(Disassembler, BadOpcodeOutOfRange) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_byte(0xAB, 1); // 超出 kOpCodeCount 的裸字节
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "<bad opcode 0xAB>");
}

TEST(Disassembler, TruncatedOperand) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::LOAD_LOCAL, 1); // u8 操作数缺失
    EXPECT_EQ(Disassembler::disassembleInstruction(&cu, 0), "LOAD_LOCAL        <truncated>");
}
