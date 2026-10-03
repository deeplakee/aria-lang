#ifndef ARIA_CODE_HPP
#define ARIA_CODE_HPP

#include "common.hpp"

namespace aria {

    enum class OpFormat : u8 {
        Simple,     // 无操作数
        U8,         // 1 字节操作数, 渲染 {:02X}
        U16,        // 2 字节小端操作数, 渲染 {:04X}
        ConstU16,   // 2 字节常量池索引(名字索引同类), 附常量可读化注释
        ImmI8,      // 1 字节有符号立即数, hex + 十进制注释
        JumpFwd,    // 2 字节前向偏移(ip+=off), 渲染 -> target; 条件跳转恒前向(if/while/短路)
        JumpBack,   // 2 字节后向偏移(ip-=off), 渲染 <- target; 后向恒无条件(while/for/for-in 回边)
        RangeFlags, // 1 字节 flags(MAKE_RANGE,位义见 kRangeFlag*), 渲染 flags=0xNN
        RegU8,      // 1 字节值寄存器索引, 附寄存器可读名注释
        Import,     // 2 字节 path 常量索引 + path 注释
    };

    // 多行宏体内 // 会因反斜杠续行吞掉下一行,表内注释只能用块注释 /* */。
#define ARIA_OPCODE_LIST(X)                \
    X(HALT, Simple)                        \
    /* ---- data loading & storage ---- */ \
    X(LOAD_CONST, ConstU16)                \
    X(LOAD_NIL, Simple)                    \
    X(LOAD_TRUE, Simple)                   \
    X(LOAD_FALSE, Simple)                  \
    X(LOAD_IMM, ImmI8)                     \
    X(LOAD_REG, RegU8)                     \
    X(LOAD_LOCAL, U16)                     \
    X(LOAD_LOCAL_1, Simple)                \
    X(LOAD_LOCAL_2, Simple)                \
    X(LOAD_LOCAL_3, Simple)                \
    X(LOAD_LOCAL_4, Simple)                \
    X(LOAD_LOCAL_5, Simple)                \
    X(LOAD_LOCAL_6, Simple)                \
    X(LOAD_LOCAL_7, Simple)                \
    X(LOAD_LOCAL_8, Simple)                \
    X(STORE_LOCAL, U16)                    \
    X(STORE_LOCAL_1, Simple)               \
    X(STORE_LOCAL_2, Simple)               \
    X(STORE_LOCAL_3, Simple)               \
    X(STORE_LOCAL_4, Simple)               \
    X(STORE_LOCAL_5, Simple)               \
    X(STORE_LOCAL_6, Simple)               \
    X(STORE_LOCAL_7, Simple)               \
    X(STORE_LOCAL_8, Simple)               \
    X(LOAD_UPVALUE, U8)                    \
    X(STORE_UPVALUE, U8)                   \
    X(CLOSE_UPVALUE, Simple)               \
    X(DEF_GLOBAL, ConstU16)                \
    X(LOAD_GLOBAL, ConstU16)               \
    X(STORE_GLOBAL, ConstU16)              \
    X(LOAD_FIELD, ConstU16)                \
    X(STORE_FIELD, ConstU16)               \
    X(LOAD_INDEX, Simple)                  \
    X(STORE_INDEX, Simple)                 \
    X(LOAD_THIS_FIELD, ConstU16)           \
    X(STORE_THIS_FIELD, ConstU16)          \
    /* ---- arithmetic & logic ---- */     \
    X(EQUAL, Simple)                       \
    X(NOT_EQUAL, Simple)                   \
    X(STRICT_EQUAL, Simple)                \
    X(STRICT_NOT_EQUAL, Simple)            \
    X(GREATER, Simple)                     \
    X(GREATER_EQUAL, Simple)               \
    X(LESS, Simple)                        \
    X(LESS_EQUAL, Simple)                  \
    X(ADD, Simple)                         \
    X(SUBTRACT, Simple)                    \
    X(MULTIPLY, Simple)                    \
    X(DIVIDE, Simple)                      \
    X(MOD, Simple)                         \
    X(NOT, Simple)                         \
    X(NEGATE, Simple)                      \
    /* ---- stack ops ---- */              \
    X(POP, Simple)                         \
    X(POP_N, U8)                           \
    X(DUP, Simple)                         \
    X(DUP2, Simple)                        \
    /* ---- debug ---- */                  \
    X(NOP, Simple)                         \
    /* ---- control flow (jumps) ---- */   \
    X(JUMP, JumpFwd)                       \
    X(JUMP_TRUE, JumpFwd)                  \
    X(JUMP_TRUE_OR_POP, JumpFwd)           \
    X(JUMP_FALSE, JumpFwd)                 \
    X(JUMP_FALSE_OR_POP, JumpFwd)          \
    X(JUMP_BACK, JumpBack)                 \
    /* fused compare-jump forms */         \
    X(JUMP_NE, JumpFwd)                    \
    /* ---- functions & closures ---- */   \
    X(CALL, U8)                            \
    X(CLOSURE, ConstU16)                   \
    /* ---- classes & objects ---- */      \
    X(MAKE_CLASS, ConstU16)                \
    X(MAKE_METHOD, ConstU16)               \
    X(MAKE_STATIC, ConstU16)               \
    X(LOAD_SUPER_FIELD, ConstU16)          \
    X(PREPARE_METHOD, ConstU16)            \
    X(CALL_METHOD, U8)                     \
    X(MAKE_LIST, U16)                      \
    X(MAKE_MAP, U16)                       \
    X(MAKE_RANGE, RangeFlags)              \
    X(BUILD_STRING, U8)                    \
    /* ---- module import ---- */          \
    X(IMPORT, Import)                      \
    /* ---- exceptions ---- */             \
    X(THROW, Simple)                       \
    /* ---- return ---- */                 \
    X(RETURN, Simple)

#define ARIA_OP_ENUM(name, format) name,
    enum class OpCode : u8 { ARIA_OPCODE_LIST(ARIA_OP_ENUM) };
#undef ARIA_OP_ENUM

#define ARIA_OP_COUNT(name, format) +1
    constexpr usize kOpCodeCount = 0 ARIA_OPCODE_LIST(ARIA_OP_COUNT);
#undef ARIA_OP_COUNT

#define ARIA_OP_NAME(name, format) #name,
    inline constexpr StringView kOpCodeNames[kOpCodeCount] = {ARIA_OPCODE_LIST(ARIA_OP_NAME)};
#undef ARIA_OP_NAME

#define ARIA_OP_FORMAT(name, format) OpFormat::format,
    inline constexpr OpFormat kOpCodeFormats[kOpCodeCount] = {ARIA_OPCODE_LIST(ARIA_OP_FORMAT)};
#undef ARIA_OP_FORMAT

#undef ARIA_OPCODE_LIST

    static_assert(std::to_underlying(OpCode::LOAD_LOCAL_8) == std::to_underlying(OpCode::LOAD_LOCAL_1) + 7,
                  "LOAD_LOCAL_1..8 must stay contiguous");
    static_assert(std::to_underlying(OpCode::STORE_LOCAL_8) == std::to_underlying(OpCode::STORE_LOCAL_1) + 7,
                  "STORE_LOCAL_1..8 must stay contiguous");

    // 仅算术/比较指令有源码算子; 落 default 即编程错误, 以 "?" 可见暴露而非静默空串。
    constexpr StringView op_symbol(const OpCode op) noexcept {
        switch (op) {
            case OpCode::ADD:
                return "+";
            case OpCode::SUBTRACT:
                return "-";
            case OpCode::MULTIPLY:
                return "*";
            case OpCode::DIVIDE:
                return "/";
            case OpCode::MOD:
                return "%";
            case OpCode::GREATER:
                return ">";
            case OpCode::GREATER_EQUAL:
                return ">=";
            case OpCode::LESS:
                return "<";
            case OpCode::LESS_EQUAL:
                return "<=";
            default:
                return "?";
        }
    }

    // MAKE_RANGE flags 位义:
    inline constexpr u8 kRangeFlagInclusive = 0x00; // 含上界(..)
    inline constexpr u8 kRangeFlagExclusive = 0x01; // 不含上界(...)
    inline constexpr u8 kRangeFlagUnbounded = 0x02; // 无上界(端点后省上界表达式, from.. / from... 同义, 含否位不编)

} // namespace aria

#endif // ARIA_CODE_HPP
