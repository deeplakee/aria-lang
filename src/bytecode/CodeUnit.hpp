#ifndef ARIA_CODEUNIT_HPP
#define ARIA_CODEUNIT_HPP

#include "bytecode/code.hpp"
#include "common.hpp"
#include "memory/Array.hpp"
#include "value/AriaArray.hpp"

namespace aria {

    class GC;

    constexpr u32 kU8OperandMax  = 0xFF;
    constexpr u32 kU16OperandMax = 0xFFFF;

    // RLE 行段: 从 offset 起至下一条 entry 的 offset 前, 字节码均属 line 行。
    struct LineEntry {
        u32 offset = 0;
        u32 line   = 0;
    };

    // 异常值 push 至 catch 参数槽(槽深恒 == stack_depth, 值直接填槽免 STORE_LOCAL)。
    struct TryRecord {
        u32 begin       = 0; // try 受保护区间起始 offset (含)
        u32 end         = 0; // try 受保护区间结束 offset (不含); [begin, end) 内的 ip 命中此记录
        u32 handle      = 0; // catch handler 入口 offset; unwind 后跳此
        u32 stack_depth = 0; // try 入口局部数(编译期快照); unwind 截值栈至 frame.slots + 此值
    };

    // 一个编译单元（函数/模块顶层）的字节码容器。
    class CodeUnit {
    public:
        Array<u8>        code;        // 字节流: opcode + 内联操作数 (小端)
        AriaArray        constants;   // 常量池 (Array<Value> + trace)
        Array<LineEntry> lines;       // RLE 行段表 (offset -> line)
        Array<TryRecord> try_records; // 异常记录表 (按 begin 非降序,允许相等)

        explicit CodeUnit(GC* gc) noexcept;

        ~CodeUnit() = default;

        CodeUnit(const CodeUnit&)            = delete;
        CodeUnit& operator=(const CodeUnit&) = delete;
        CodeUnit(CodeUnit&&)                 = delete;
        CodeUnit& operator=(CodeUnit&&)      = delete;

        // emit (带 line; RLE 行号记录收口于此)
        void emit_byte(u8 byte, u32 line);

        void emit_word(u16 word, u32 line); // 小端: 低字节先

        void emit_op(OpCode op, u32 line);

        // 分块 emit POP_N(每块<=255);chunk==1 时降级为 POP(1B,免操作数)。
        void emit_pop_n(u32 count, u32 line);

        // 发 op + 占位 u16, 返回占位偏移 src_off; 无越界。
        u32 emit_jump(OpCode op, u32 line);

        // 前向回填 src_off 处占位为 target_off - base_off(base_off = 读完 u16 操作数后的 ip,即偏移基准);
        //   越界(>65535)返 false(不写),成功返 true。
        bool patch_jump(u32 src_off);

        // 后向:emit JUMP_BACK + (base_off - target_off)(偏移基准同 patch_jump);
        //   越界(反向/超 64KB)emit 占位 word 0 后返 false。
        bool emit_jump_back(u32 target_off, u32 line);

        // 局部槽 load/store:slot 1..8 发零操作数 N 短变体(LOAD/STORE_LOCAL_k,槽号内嵌枚举名),
        // 否则通用形态(LOAD/STORE_LOCAL + u16 槽号,含槽 0)。
        void emit_load_local(u16 slot, u32 line);

        void emit_store_local(u16 slot, u32 line);

        // 当前字节流长度(= 下一条 emit 的 offset)。
        [[nodiscard]]
        u32 size() const noexcept {
            return static_cast<u32>(code.size());
        }

        // 只追加、不去重; 索引 u16, 超 65535 即断言。
        u16 add_constant(Value value);

        // 查 offset 所属行号(最大 entry.offset <= offset 的 line); 空表或先于首条返 0。
        [[nodiscard]]
        u32 line_for_offset(u32 offset) const noexcept;

        // 按 ip 查最近覆盖的 try 记录(嵌套取最内层), 无覆盖返 nullopt。
        // 记录按 begin 非降序(允许相等), 二分 + 前溯, O(嵌套深度) 最坏。
        [[nodiscard]]
        Opt<const TryRecord*> find_try_handler(u32 ip) const noexcept;

        void trace(GC& gc) const noexcept { constants.trace(gc); }

        // name 用作反汇编输出表头(函数名/模块名)。
        [[nodiscard]]
        String disassemble(StringView name) const;

    private:
        // 在当前 code.size() 偏移处按 line 记一条 RLE 行段;与末条同行则不追加。
        void record_line_(u32 line) noexcept;
    };

} // namespace aria

#endif // ARIA_CODEUNIT_HPP
