#include "Disassembler.hpp"

#include <format>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "runtime/value_register.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // u8 -> u32(零扩展,值不变):std::format 对 u8(unsigned char)默认按字符打印,转 u32 才能按数字格式化(如 {:02X})。
        u32 to_u32(const u8 value) noexcept { return static_cast<u32>(value); }

        // i8 -> i32(符号扩展):LOAD_IMM 的 8 位有符号立即数转十进制注释用。
        i32 to_i32(const i8 value) noexcept { return static_cast<i32>(value); }

        // 取常量池索引处的 Value(越界返 nullopt)。只读 cu,不推进游标。
        Opt<Value> constant_at(const CodeUnit* cu, const u16 idx) {
            if (static_cast<usize>(idx) >= cu->constants.size()) {
                return std::nullopt;
            }
            return cu->constants[idx];
        }

        // 值可读化统一走 value/Value.hpp 的 format_value_debug(非重入渲染,契约详见
        // Object.hpp/Value.hpp 注释);字符串走 ObjString::debug_repr 的带引号转义形态。

        // 把 opcode 名与操作数段拼成一行:左对齐 16 列的 op_name,空操作数即裸名,末尾尾随空格裁掉。
        String join_line(const StringView op_name, const StringView operands) {
            String line = std::format("{:<16}", op_name);
            if (!operands.empty()) {
                line.append("  ").append(operands);
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            return line;
        }

        String simple_instruction(const StringView op_name) { return std::format("{}", op_name); }

    } // namespace

    // 读 1 字节(调用方已预检越界),推进 offset_。
    u8 Disassembler::read_u8() noexcept { return codeunit_->code[offset_++]; }

    // 读 2 字节小端 u16(调用方已预检越界),推进 offset_。拼装走 util::make_u16。
    u16 Disassembler::read_u16() noexcept {
        const u16 value = util::make_u16(codeunit_->code[offset_], codeunit_->code[offset_ + 1]);
        offset_ += 2;
        return value;
    }

    // 操作数读取越界(残缺字节码)时:把 offset_ 推到末尾停解码,返回 <truncated> 作为操作数段。
    String Disassembler::truncated() noexcept {
        offset_ = static_cast<u32>(codeunit_->code.size());
        return "<truncated>";
    }

    // 越界预检:读取 need 字节是否会越过末尾(残缺字节码将被截断)。
    bool Disassembler::is_truncated(const u32 need) const noexcept { return offset_ + need > codeunit_->code.size(); }

    // 越界退化为 <bad idx>(消费面见 Disassembler.hpp 注)。
    String Disassembler::format_constant(const u16 idx) const {
        const auto v = constant_at(codeunit_, idx);
        return v ? format_value_debug(*v) : std::format("<bad idx {}>", idx);
    }

    // 单字节 i8 立即数(LOAD_IMM):`{:02X}` 操作数 + 有符号十进制注释。
    String Disassembler::load_imm(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        const u8   raw = read_u8();
        const auto imm = std::bit_cast<i8>(raw);
        return join_line(op_name, std::format("{:02X}  ; {}", to_u32(raw), to_i32(imm)));
    }

    String Disassembler::u8_instruction(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        return join_line(op_name, std::format("{:02X}", to_u32(read_u8())));
    }

    // LOAD_REG:u8 寄存器索引 + 寄存器可读名注释(名字表与索引常量同源,value_register.hpp)。
    String Disassembler::register_instruction(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        const u8 raw = read_u8();
        if (raw >= kValueRegisterCount) {
            return join_line(op_name, std::format("{:02X}  ; <bad reg {}>", to_u32(raw), to_u32(raw)));
        }
        return join_line(op_name, std::format("{:02X}  ; {}", to_u32(raw), to_string(static_cast<ValueRegister>(raw))));
    }

    String Disassembler::u16_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        return join_line(op_name, std::format("{:04X}", read_u16()));
    }

    // MAKE_RANGE 的 flags:u8 位义见 code.hpp kRangeFlagInclusive/Exclusive/Unbounded;渲染
    // 原始值 + 解码名供对照。
    String Disassembler::make_range(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        const u8 raw = read_u8();
        return join_line(op_name, std::format("{:02X}  ; flags=0x{:02X}", to_u32(raw), to_u32(raw)));
    }

    // 常量/名字索引:{:04X} 操作数 + 常量可读化注释(越界 <bad idx>)。
    String Disassembler::const_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 idx = read_u16();
        return join_line(op_name, std::format("{:04X}  ; {}", idx, format_constant(idx)));
    }

    // 前向跳转(JUMP 及条件变体):`{:04X} -> target`,目标 = 读操作数后 offset_ + off。
    String Disassembler::jump_forward(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 off    = read_u16();
        const i64 target = static_cast<i64>(offset_) + off;
        return join_line(op_name, std::format("{:04X} -> {:04X}", off, static_cast<u32>(target)));
    }

    // 后向跳转(JUMP_BACK):`{:04X} <- target`,目标 = 读操作数后 offset_ - off。
    String Disassembler::jump_back(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 off    = read_u16();
        const i64 target = static_cast<i64>(offset_) - off;
        return join_line(op_name, std::format("{:04X} <- {:04X}", off, static_cast<u32>(target)));
    }

    // IMPORT:u16 path 操作数 + path 名注释(绑定由后续指令完成,故无 alias 操作数)。
    String Disassembler::import_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16    path_idx = read_u16();
        const String path     = format_constant(path_idx);
        return join_line(op_name, std::format("{:04X}  ; {}", path_idx, path));
    }

    // 反汇编单条指令,返回指令文本(不含偏移前缀/换行),推进 offset_ 越过该指令。
    // 表驱动按格式分发(见 Disassembler.hpp 文档)。
    String Disassembler::dis_instruction() {
        const u8 byte = codeunit_->code[offset_++];

        // 越界 opcode 字节(无对应枚举)容错:直接报 bad opcode 并停解码。
        if (byte >= kOpCodeCount) {
            return std::format("<bad opcode 0x{:02X}>", to_u32(byte));
        }

        const StringView op_name = kOpCodeNames[byte];
        switch (kOpCodeFormats[byte]) {
            case OpFormat::Simple:
                return simple_instruction(op_name);
            case OpFormat::U8:
                return u8_instruction(op_name);
            case OpFormat::U16:
                return u16_instruction(op_name);
            case OpFormat::ConstU16:
                return const_instruction(op_name);
            case OpFormat::ImmI8:
                return load_imm(op_name);
            case OpFormat::JumpFwd:
                return jump_forward(op_name);
            case OpFormat::JumpBack:
                return jump_back(op_name);
            case OpFormat::RangeFlags:
                return make_range(op_name);
            case OpFormat::RegU8:
                return register_instruction(op_name);
            case OpFormat::Import:
                return import_instruction(op_name);
        }
        UNREACHABLE();
    }

    Disassembler::Disassembler(const CodeUnit* codeunit, const StringView name) :
        codeunit_{codeunit}, name_{name}, offset_{0} {}

    // 详见 Disassembler.hpp 的文档注释(输出结构/小节划分)。
    String Disassembler::disassemble() {
        offset_    = 0;
        String out = std::format("== {} ==\n", name_);

        // 常量池小节(非空才列):便于解读常量索引操作数。
        if (!codeunit_->constants.empty()) {
            out += "\nconstants:\n";
            for (usize i = 0; i < codeunit_->constants.size(); ++i) {
                out += std::format("  {:04X}: {}\n", static_cast<u32>(i), format_value_debug(codeunit_->constants[i]));
            }
        }

        // 异常记录表小节(非空才列):字段语义见 TryRecord(CodeUnit.hpp);记录按 begin 升序,
        // 与 code 段偏移对照阅读。
        if (const auto& records = codeunit_->try_records; !records.empty()) {
            out += "\ntry records:\n";
            for (usize i = 0; i < records.size(); ++i) {
                const auto& [begin, end, handle, stack_depth] = records[i];
                out += std::format("  {:04X}: [{:04X}, {:04X}) handle={:04X} stack_depth={}\n", static_cast<u32>(i),
                                   begin, end, handle, stack_depth);
            }
        }

        // code 段:始终列出。每行:偏移(4 hex) + 行号(与上行同号用 '|' 占位) + 指令文本。
        // 先存 ip 再解码,避免 format 实参求值顺序干扰 offset_。
        out += "\ncode:\n";
        Opt<u32> prev_line = std::nullopt;
        while (offset_ < codeunit_->code.size()) {
            const u32    ip       = offset_;
            const u32    line     = codeunit_->line_for_offset(ip);
            const String line_col = prev_line == line ? String{"   |"} : std::format("{:>4}", line);
            prev_line             = line;
            out += std::format("{:04X} {} {}\n", ip, line_col, dis_instruction());
        }
        out += "\n== end ==\n";
        return out;
    }

    String Disassembler::disassembleCodeUnit(const CodeUnit* codeunit, const StringView name) {
        return Disassembler{codeunit, name}.disassemble();
    }

    String Disassembler::disassembleInstruction(const CodeUnit* codeunit, const u32 offset) {
        // 构造一次性 Disassembler,把内部游标拨到 offset,解码单条指令后丢弃。不依赖 name(disassemble
        // 才用),故传空。offset 合法性由调用方保证(VM 执行跟踪处 ip 必指向有效 opcode)。
        Disassembler d{codeunit, {}};
        d.offset_ = offset;
        return d.dis_instruction();
    }

} // namespace aria
