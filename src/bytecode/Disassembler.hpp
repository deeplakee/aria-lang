#ifndef ARIA_DISASSEMBLER_HPP
#define ARIA_DISASSEMBLER_HPP

#include "common.hpp"

namespace aria {

    class CodeUnit;

    // 字节码反汇编器: 把 CodeUnit 字节流解码为可读文本。
    class Disassembler {
    public:
        // name 为表头标识(函数名/模块名), 须在 disassemble() 期间存活。
        explicit Disassembler(const CodeUnit* codeunit, StringView name);

        // 反汇编整个 CodeUnit; 每次调用从头解码(复位游标), 对象可复用。
        [[nodiscard]]
        String disassemble();

        [[nodiscard]]
        static String disassembleCodeUnit(const CodeUnit* codeunit, StringView name);

        // 反汇编 offset 处单条指令, 文本与 disassemble() 每行的指令段一致(不含偏移前缀/行号/换行)。
        // offset 须 < code.size()。
        [[nodiscard]]
        static String disassembleInstruction(const CodeUnit* codeunit, u32 offset);

    private:
        const CodeUnit* codeunit_;
        StringView      name_;
        u32             offset_; // 解码游标

        [[nodiscard]]
        u8 read_u8() noexcept;

        [[nodiscard]]
        u16 read_u16() noexcept;

        [[nodiscard]]
        String truncated() noexcept;

        [[nodiscard]]
        bool is_truncated(u32 need) const noexcept;

        // 常量可读化注释(只读, 不推进 offset_)。
        [[nodiscard]]
        String format_constant(u16 idx) const;

        String dis_instruction();

        String load_imm(StringView op_name);

        String u8_instruction(StringView op_name);

        String u16_instruction(StringView op_name);

        String make_range(StringView op_name);

        String const_instruction(StringView op_name);

        String jump_forward(StringView op_name);

        String jump_back(StringView op_name);

        String register_instruction(StringView op_name);

        String import_instruction(StringView op_name);
    };

} // namespace aria

#endif // ARIA_DISASSEMBLER_HPP
