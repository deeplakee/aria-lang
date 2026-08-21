#ifndef ARIA_CODE_HPP
#define ARIA_CODE_HPP

#include "common.hpp"

namespace aria {
    enum class OpCode : u8 {
        HALT = 0,
        // Data loading and storage
        LOAD_CONST,
        LOAD_NIL,
        LOAD_TRUE,
        LOAD_FALSE,
        LOAD_IMM,

        LOAD_LOCAL,
        STORE_LOCAL,
        LOAD_LOCAL_L,  // 长变体: slot u16, slot>=256 时用; 见 bytecode-instruction-set.md §2.3
        STORE_LOCAL_L, // 长变体: 同 STORE_LOCAL, slot u16

        LOAD_UPVALUE,
        STORE_UPVALUE,
        CLOSE_UPVALUE,

        DEF_GLOBAL,
        LOAD_GLOBAL,
        STORE_GLOBAL,

        LOAD_FIELD,
        STORE_FIELD,

        LOAD_INDEX,
        STORE_INDEX,

        LOAD_THIS_FIELD,
        STORE_THIS_FIELD,

        // Arithmetic and logical operations
        EQUAL,
        NOT_EQUAL,
        STRICT_EQUAL,
        STRICT_NOT_EQUAL,
        GREATER,
        GREATER_EQUAL,
        LESS,
        LESS_EQUAL,
        ADD,
        SUBTRACT,
        MULTIPLY,
        DIVIDE,
        MOD,
        NOT,
        NEGATE,

        // Stack operations
        POP,
        POP_N,
        DUP,  // 复制栈顶 1 个槽:[a] -> [a, a]
        DUP2, // 复制栈顶 2 个槽(保序):[a, b] -> [a, b, a, b]

        // Output/Debug
        PRINT,
        NOP,

        // Control flow (jump and branch)
        // 偏移 u16 无符号, 方向编码于 opcode: JUMP* 前向 (ip+=off), JUMP_BACK 后向 (ip-=off)
        // 后向恒无条件 (while/for/for-in 回边), 条件跳转恒前向; 见 bytecode-instruction-set.md §2.3/§4.12
        JUMP,
        JUMP_TRUE,
        JUMP_TRUE_OR_POP, // 短路 ||: 真跳留值(结果即 v), 否则弹落空
        JUMP_FALSE,
        JUMP_FALSE_OR_POP, // 短路 &&: 假跳留值(结果即 v), 否则弹落空
        JUMP_BACK,         // 后向无条件回边

        // Functions and Closures
        CALL,
        CLOSURE,

        // Classes and Objects
        LOAD_OBJECT,
        MAKE_CLASS,
        MAKE_METHOD,
        MAKE_STATIC,
        LOAD_SUPER_METHOD,
        INVOKE_METHOD, // 预备指令(暂 pass): 合并"取方法 + 调用"; 当前模型无法编译期区分方法调用与属性访问, 暂不发射,
                       // 留作 VM 性能 pass
        MAKE_LIST,
        MAKE_MAP,
        MAKE_RANGE, // 区间构造: [lo, hi] -> [range]; 操作数 flags:u8 编码含/不含上界(.. vs ...)

        // Module import
        IMPORT,

        // exception handling
        THROW,

        // return
        RETURN,
    };
} // namespace aria

#endif // ARIA_CODE_HPP
