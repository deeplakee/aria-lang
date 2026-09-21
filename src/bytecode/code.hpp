#ifndef ARIA_CODE_HPP
#define ARIA_CODE_HPP

#include "common.hpp"

namespace aria {

    // 操作数格式类别:ARIA_OPCODE_LIST 第二列的取值域,Disassembler 按此分发解码,
    // 与 bytecode-instruction-set.md §2.2 位宽表一一对应。新增格式类别须同步 Disassembler
    // 的格式分发 switch(仅当既有类别容纳不下时才需要)。
    enum class OpFormat : u8 {
        Simple,     // 无操作数
        U8,         // 1 字节操作数, 渲染 {:02X}
        U16,        // 2 字节小端操作数, 渲染 {:04X}
        ConstU16,   // 2 字节常量池索引(名字索引同类), 附常量可读化注释
        ImmI8,      // 1 字节有符号立即数, hex + 十进制注释
        JumpFwd,    // 2 字节前向偏移(ip+=off), 渲染 -> target; 条件跳转恒前向(if/while/短路), 见 instruction-set §2.3
        JumpBack,   // 2 字节后向偏移(ip-=off), 渲染 <- target; 后向恒无条件(while/for/for-in 回边)
        RangeFlags, // 1 字节 flags(MAKE_RANGE,位义见 kRangeFlag*), 渲染 flags=0xNN
        RegU8,      // 1 字节值寄存器索引, 附寄存器可读名注释(注册表 value_register.hpp)
        Import,     // 2 字节 path 常量索引 + path 注释
    };

    // 指令集单一事实源: 每行 X(枚举名, 操作数格式), 枚举顺序即 opcode 数值(首条 HALT 隐式为 0,
    // 依赖稠密递增)。OpCode / kOpCodeCount / kOpCodeNames / kOpCodeFormats 均由本表展开生成
    // (生成器宏用完即 #undef); 未来 computed goto 跳转表可同表再加一行消费(见 vm-design.md)。
    //
    // 新增指令流程: 本表加一行(选既有 OpFormat 类别) -> AriaVM 加对应 case -> 文档
    // bytecode-instruction-set.md 同步; Disassembler 与名字/格式表零改动。
    // 续行符对齐交给 clang-format(RightAlignEscapedNewlines), 无需手工维护。
    //
    // 表内不放说明性注释, 语义细节统一见 bytecode-instruction-set.md §4。易踩点速览:
    //   - LOAD_IMM: u8 操作数按 i8 位型重解释做符号扩展(发射侧先经 i8 再转 u8)
    //   - PREPARE_METHOD/CALL_METHOD: 两段式方法调用(解析先于实参求值, 见 §5.6); 待调值槽由
    //     实参整体下移一格补掉, 调用区恒为 [recv, a1..aN]
    // 若确需表内注释, 只能用块注释 /* */ -- 多行宏体内 // 会因反斜杠续行吞掉下一行。
#define ARIA_OPCODE_LIST(X)                \
    X(HALT, Simple)                        \
    /* ---- data loading & storage ---- */ \
    X(LOAD_CONST, ConstU16)                \
    X(LOAD_NIL, Simple)                    \
    X(LOAD_TRUE, Simple)                   \
    X(LOAD_FALSE, Simple)                  \
    X(LOAD_IMM, ImmI8)                     \
    X(LOAD_REG, RegU8)                     \
    X(LOAD_LOCAL, U8)                      \
    X(STORE_LOCAL, U8)                     \
    X(LOAD_LOCAL_L, U16)                   \
    X(STORE_LOCAL_L, U16)                  \
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
    /* ---- output & debug ---- */         \
    X(PRINT, Simple)                       \
    X(NOP, Simple)                         \
    /* ---- control flow (jumps) ---- */   \
    X(JUMP, JumpFwd)                       \
    X(JUMP_TRUE, JumpFwd)                  \
    X(JUMP_TRUE_OR_POP, JumpFwd)           \
    X(JUMP_FALSE, JumpFwd)                 \
    X(JUMP_FALSE_OR_POP, JumpFwd)          \
    X(JUMP_BACK, JumpBack)                 \
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
    /* ---- module import ---- */          \
    X(IMPORT, Import)                      \
    /* ---- exceptions ---- */             \
    X(THROW, Simple)                       \
    /* ---- return ---- */                 \
    X(RETURN, Simple)

#define ARIA_OP_ENUM(name, format) name,
    enum class OpCode : u8 { ARIA_OPCODE_LIST(ARIA_OP_ENUM) };
#undef ARIA_OP_ENUM

    // 指令总数(= X 表行数):opcode 字节越界判定等用,替代对枚举稠密(上界 = 末条枚举值)的依赖。
#define ARIA_OP_COUNT(name, format) +1
    constexpr usize kOpCodeCount = 0 ARIA_OPCODE_LIST(ARIA_OP_COUNT);
#undef ARIA_OP_COUNT

    // opcode -> 名字 / 操作数格式查表(数组显式以 kOpCodeCount 定界,表行数与枚举条数不一致即编译错)。
    // 供 Disassembler 等冷路径消费;VM 热路径不查表(操作数读取内联在各 case)。
#define ARIA_OP_NAME(name, format) #name,
    inline constexpr StringView kOpCodeNames[kOpCodeCount] = {ARIA_OPCODE_LIST(ARIA_OP_NAME)};
#undef ARIA_OP_NAME

#define ARIA_OP_FORMAT(name, format) OpFormat::format,
    inline constexpr OpFormat kOpCodeFormats[kOpCodeCount] = {ARIA_OPCODE_LIST(ARIA_OP_FORMAT)};
#undef ARIA_OP_FORMAT

#undef ARIA_OPCODE_LIST

    // MAKE_RANGE flags 位义:0x00 含上界(..)、0x01 不含上界(...)、0x02 无上界(端点后省
    // 上界表达式,from.. / from... 同义,含否位不编)。编译发射与 VM 解码同源。
    inline constexpr u8 kRangeFlagInclusive = 0x00;
    inline constexpr u8 kRangeFlagExclusive = 0x01;
    inline constexpr u8 kRangeFlagUnbounded = 0x02;

} // namespace aria

#endif // ARIA_CODE_HPP
