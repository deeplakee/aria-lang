#ifndef ARIA_DISASSEMBLER_HPP
#define ARIA_DISASSEMBLER_HPP

#include "common.hpp"

namespace aria {

    class CodeUnit;

    // 字节码反汇编器:把 CodeUnit 字节流解码为可读文本,供调试/调试器/测试核对。
    //
    //   与 VM 共用「opcode -> 操作数格式」解码约定(.claude/reference/bytecode/bytecode-instruction-set.md §2.1/§8);
    //   解码表当前内联于 .cpp,VM 落地后可提取共享。
    //
    //   实例持一个解码游标(codeunit_ + offset_),disassemble() 从头解码整个 CodeUnit;
    //   静态入口 disassembleCodeUnit(codeunit, name) 便捷包装为 `Disassembler{...}.disassemble()`。
    //
    //   disassemble() 返回:表头 `== name ==` + `constants:` 小节(非空才列)
    //   + `code:` 小节(逐指令,始终列出)+ 结尾 `== end ==`;段间空行分隔。
    //   每条指令一行:偏移(4 hex) | 行号(右对齐 4 列十进制,与上行同号用 '|' 占位)
    //   | opcode 名 | 操作数(hex) | ; 解析注释(常量值/名字/跳转目标/立即数)。
    class Disassembler {
    public:
        // 绑定待反汇编的 CodeUnit 与表头标识 name(函数名/模块名);name 须在 disassemble() 期间存活。
        explicit Disassembler(const CodeUnit* codeunit, StringView name);

        // 反汇编整个 CodeUnit,返回带表头与逐指令文本的字符串。每次调用从头解码(复位游标),对象可复用。
        [[nodiscard]]
        String disassemble();

        // 静态便捷入口:等价 `Disassembler{codeunit, name}.disassemble()`。
        [[nodiscard]]
        static String disassembleCodeUnit(const CodeUnit* codeunit, StringView name);

    private:
        const CodeUnit* codeunit_;
        StringView      name_;
        usize           offset_; // 解码游标,指向当前待解码字节

        // ---- 底层读取(推进 offset_) ----
        [[nodiscard]]
        u8 read_u8() noexcept;
        [[nodiscard]]
        u16 read_u16() noexcept;
        [[nodiscard]]
        String truncated() noexcept;
        [[nodiscard]]
        bool is_truncated(usize need) const noexcept;
        // 常量索引的注释渲染(只读,不推进 offset_;名字索引与 LOAD_CONST/CLOSURE 共用)。
        [[nodiscard]]
        String format_constant(u16 idx) const;

        // ---- 逐指令解码(推进 offset_) ----
        String dis_instruction();
        String load_imm(StringView op_name);
        String u8_instruction(StringView op_name);
        String u16_instruction(StringView op_name);
        String make_range(StringView op_name);
        String const_instruction(StringView op_name);
        String name_instruction(StringView op_name);
        String jump_forward(StringView op_name);
        String jump_back(StringView op_name);
        String import_instruction(StringView op_name);
        String invoke_instruction(StringView op_name);
    };

} // namespace aria

#endif // ARIA_DISASSEMBLER_HPP
