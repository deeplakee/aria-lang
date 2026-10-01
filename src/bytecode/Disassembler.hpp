#ifndef ARIA_DISASSEMBLER_HPP
#define ARIA_DISASSEMBLER_HPP

#include "common.hpp"

namespace aria {

    class CodeUnit;

    // 字节码反汇编器:把 CodeUnit 字节流解码为可读文本,供调试/调试器/测试核对。解码表驱动:opcode 名与操作数格式查
    // code.hpp 的 X 表生成物(kOpCodeNames/kOpCodeFormats),按格式分发到解码函数;新增指令只需在 ARIA_OPCODE_LIST
    // 加一行,本类零改动(仅当需新操作数格式类别时才同步分发 switch)。实例持一个解码游标(codeunit_ + offset_),
    // disassemble() 从头解码整个 CodeUnit;静态入口 disassembleCodeUnit(codeunit, name) 便捷包装。disassemble()
    // 输出:表头 + `constants:`/`try records:` 小节(非空才列)+ `code:` 小节(逐指令, 始终列出)+ 结尾;每条指令
    // 一行(偏移 | 行号 | opcode 名 | 操作数 | 解析注释)。
    class Disassembler {
    public:
        // 绑定待反汇编的 CodeUnit 与表头标识 name(函数名/模块名);name 须在 disassemble() 期间存活。
        explicit Disassembler(const CodeUnit* codeunit, StringView name);

        // 反汇编整个 CodeUnit,返回带表头与逐指令文本的字符串。每次调用从头解码(复位游标),对象可复用。
        [[nodiscard]]
        String disassemble();

        // 静态便捷入口:一次性构造并反汇编整个 CodeUnit。
        [[nodiscard]]
        static String disassembleCodeUnit(const CodeUnit* codeunit, StringView name);

        // 反汇编 codeunit 中 offset 处的**单条指令**,返回该指令的可读文本(与 disassemble() 中
        // 每行的指令段一致,不含偏移前缀/行号/换行)。仅推进内部游标越过该指令(不动调用方 offset)。
        // offset 须 < code.size()。供 VM 执行跟踪(DEBUG_TRACE_EXECUTION)逐指令打印,免为跟踪复制解码表。
        [[nodiscard]]
        static String disassembleInstruction(const CodeUnit* codeunit, u32 offset);

    private:
        const CodeUnit* codeunit_;
        StringView      name_;
        u32             offset_; // 解码游标,指向当前待解码字节

        // 底层读取(推进 offset_)
        [[nodiscard]]
        u8 read_u8() noexcept;
        [[nodiscard]]
        u16 read_u16() noexcept;
        [[nodiscard]]
        String truncated() noexcept;
        [[nodiscard]]
        bool is_truncated(u32 need) const noexcept;
        // 常量索引的注释渲染(只读,不推进 offset_;名字索引格式 ConstU16 与 LOAD_CONST/CLOSURE 共用)。
        [[nodiscard]]
        String format_constant(u16 idx) const;

        // 逐指令解码(推进 offset_)
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
