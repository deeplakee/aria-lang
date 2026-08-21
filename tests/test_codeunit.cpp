#include <gtest/gtest.h>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "object/ObjString.hpp"
#include "value/Value.hpp"

using aria::CodeUnit;
using aria::GC;
using aria::new_string;
using aria::ObjString;
using aria::OpCode;
using aria::TryRecord;
using aria::u16;
using aria::u8;
using aria::usize;
using aria::Value;

namespace {
    // 小端读 2 字节(测试辅助: CodeUnit 不再提供 read_word, 裸字段移位)。
    [[nodiscard]] u16 read_word_le(const CodeUnit& cu, usize off) {
        return static_cast<u16>(static_cast<u16>(cu.code[off]) | (static_cast<u16>(cu.code[off + 1]) << 8));
    }
} // namespace

TEST(CodeUnit, EmptyState) {
    GC       gc;
    CodeUnit cu{&gc};
    EXPECT_EQ(cu.code.size(), 0u);
    EXPECT_TRUE(cu.code.empty());
    EXPECT_EQ(cu.constants.size(), 0u);
    EXPECT_EQ(cu.line_for_offset(0), 0u); // 空行号表 -> 0(未知行)
}

TEST(CodeUnit, EmitByteSequence) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_byte(0xAA, 1);
    cu.emit_byte(0xBB, 1);
    cu.emit_byte(0xCC, 1);
    EXPECT_EQ(cu.code.size(), 3u);
    EXPECT_EQ(cu.code[0], 0xAA);
    EXPECT_EQ(cu.code[1], 0xBB);
    EXPECT_EQ(cu.code[2], 0xCC);
}

TEST(CodeUnit, EmitWordIsLittleEndian) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_word(0x1234, 7);
    EXPECT_EQ(cu.code.size(), 2u);
    EXPECT_EQ(cu.code[0], 0x34); // 低字节先
    EXPECT_EQ(cu.code[1], 0x12);
    EXPECT_EQ(read_word_le(cu, 0), 0x1234);
}

TEST(CodeUnit, EmitOpWritesOpcodeByte) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::ADD, 1);
    cu.emit_op(OpCode::RETURN, 1);
    EXPECT_EQ(cu.code[0], static_cast<u8>(OpCode::ADD));
    EXPECT_EQ(cu.code[1], static_cast<u8>(OpCode::RETURN));
}

TEST(CodeUnit, LineTableRleDedupSameLine) {
    // 同行连续 emit 只产生 1 条 RLE 行段
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_byte(0x00, 1);
    cu.emit_byte(0x01, 1);
    cu.emit_byte(0x02, 1);
    for (usize i = 0; i < 3; ++i) {
        EXPECT_EQ(cu.line_for_offset(i), 1u);
    }
}

TEST(CodeUnit, LineTableAppendsOnLineChange) {
    // 换行追加新行段; 行号可回退(仅 offset 单调, line 不要求单调)
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_byte(0xAA, 1); // offset 0, line 1
    cu.emit_byte(0xBB, 1); // offset 1, line 1 (去重)
    cu.emit_byte(0xCC, 1); // offset 2, line 1 (去重)
    cu.emit_byte(0xDD, 2); // offset 3, line 2
    cu.emit_byte(0xEE, 1); // offset 4, line 1 (回退, 仍追加新行段)

    EXPECT_EQ(cu.line_for_offset(0), 1u);
    EXPECT_EQ(cu.line_for_offset(1), 1u);
    EXPECT_EQ(cu.line_for_offset(2), 1u);
    EXPECT_EQ(cu.line_for_offset(3), 2u);
    EXPECT_EQ(cu.line_for_offset(4), 1u);
}

TEST(CodeUnit, LineForOffsetBinarySearch) {
    // 多次换行验证二分查找: lines = [{0,1},{2,2},{3,3},{4,1}]
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_word(0x1111, 1); // offset 0,1 -- line 1 (word 两字节同行)
    cu.emit_byte(0x22, 2);   // offset 2 -- line 2
    cu.emit_byte(0x33, 3);   // offset 3 -- line 3
    cu.emit_byte(0x44, 1);   // offset 4 -- line 1 (回退)

    EXPECT_EQ(cu.line_for_offset(0), 1u);
    EXPECT_EQ(cu.line_for_offset(1), 1u);
    EXPECT_EQ(cu.line_for_offset(2), 2u);
    EXPECT_EQ(cu.line_for_offset(3), 3u);
    EXPECT_EQ(cu.line_for_offset(4), 1u);
}

TEST(CodeUnit, ConstantPool) {
    GC        gc;
    CodeUnit  cu{&gc};
    const u16 i0 = cu.add_constant(Value::nil_val());
    const u16 i1 = cu.add_constant(Value::from_i32(42));
    const u16 i2 = cu.add_constant(Value::from_f64(3.14));
    EXPECT_EQ(i0, 0u);
    EXPECT_EQ(i1, 1u);
    EXPECT_EQ(i2, 2u);
    EXPECT_EQ(cu.constants.size(), 3u);
    EXPECT_TRUE(cu.constants[0].is_nil());
    EXPECT_EQ(cu.constants[1].as_int(), 42);
    EXPECT_DOUBLE_EQ(cu.constants[2].as_f64(), 3.14);
}

TEST(CodeUnit, PatchWordBackfillKeepsLineTable) {
    // 跳转占位 + 回填: patch 不动行号表
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_op(OpCode::JUMP_FALSE, 5);  // offset 0, line 5
    const usize patch = cu.code.size(); // = 1
    cu.emit_word(0, 5);                 // offset 1,2 占位, line 5
    EXPECT_EQ(patch, 1u);

    // 回填: 小端覆写 2 字节(裸字段)
    cu.code[patch]     = static_cast<u8>(0xABCD & 0xFF);
    cu.code[patch + 1] = static_cast<u8>((0xABCD >> 8) & 0xFF);
    EXPECT_EQ(read_word_le(cu, patch), 0xABCD);
    EXPECT_EQ(cu.code[patch], 0xCD);
    EXPECT_EQ(cu.code[patch + 1], 0xAB);
    // 行号表未变: 全部仍在 line 5
    EXPECT_EQ(cu.line_for_offset(0), 5u);
    EXPECT_EQ(cu.line_for_offset(1), 5u);
    EXPECT_EQ(cu.line_for_offset(2), 5u);
}

TEST(CodeUnit, PatchByte) {
    GC       gc;
    CodeUnit cu{&gc};
    cu.emit_byte(0x00, 1);
    cu.emit_byte(0x00, 1);
    cu.code[0] = 0xFF;
    EXPECT_EQ(cu.code[0], 0xFF);
    EXPECT_EQ(cu.code[1], 0x00);
}

TEST(CodeUnit, TraceMarksObjectConstants) {
    // trace 委托 constants.trace: 遍历常量标其中的对象
    GC         gc;
    CodeUnit   cu{&gc};
    ObjString* s = new_string(gc, "hello");
    cu.add_constant(Value::nil_val());   // 非对象: mark 无副作用
    cu.add_constant(Value::from_i32(7)); // 非对象
    cu.add_constant(Value::from_obj(s)); // 对象: 应被标
    EXPECT_FALSE(s->is_marked());
    cu.trace(gc);
    EXPECT_TRUE(s->is_marked());
}

TEST(CodeUnit, TryRecordFindHandler) {
    GC       gc;
    CodeUnit cu{&gc};
    // 空表
    EXPECT_FALSE(cu.find_try_handler(0).has_value());

    // A: [10, 100) handle=200; B: [30, 60) handle=150 (嵌套在 A 内)
    cu.try_records.push(TryRecord{.begin = 10, .end = 100, .handle = 200});
    cu.try_records.push(TryRecord{.begin = 30, .end = 60, .handle = 150});

    EXPECT_FALSE(cu.find_try_handler(0).has_value());   // 区间前
    EXPECT_EQ(cu.find_try_handler(10).value(), 200u);   // A 起点(含)
    EXPECT_EQ(cu.find_try_handler(29).value(), 200u);   // A 内、B 前
    EXPECT_EQ(cu.find_try_handler(30).value(), 150u);   // B 起点(最内层)
    EXPECT_EQ(cu.find_try_handler(59).value(), 150u);   // B 内
    EXPECT_EQ(cu.find_try_handler(60).value(), 200u);   // B 结束(不含) -> 回到 A
    EXPECT_EQ(cu.find_try_handler(99).value(), 200u);   // A 内
    EXPECT_FALSE(cu.find_try_handler(100).has_value()); // A 结束(不含)
    EXPECT_FALSE(cu.find_try_handler(500).has_value()); // 区间外
}
