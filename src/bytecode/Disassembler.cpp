#include "Disassembler.hpp"

#include <format>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "runtime/value_register.hpp"
#include "util/util.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // std::format 将 u8 按字符打印, 转 u32 才能按数字格式化。
        u32 to_u32(const u8 value) noexcept { return static_cast<u32>(value); }

        // i8 符号扩展, 理由同 to_u32(std::format 按字符打印窄整型)。
        i32 to_i32(const i8 value) noexcept { return static_cast<i32>(value); }

        Opt<Value> constant_at(const CodeUnit* cu, const u16 idx) {
            if (static_cast<usize>(idx) >= cu->constants.size()) {
                return std::nullopt;
            }
            return cu->constants[idx];
        }

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

    // 读 2 字节小端 u16(调用方已预检越界), 推进 offset_。
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

    String Disassembler::format_constant(const u16 idx) const {
        const auto v = constant_at(codeunit_, idx);
        return v ? format_value_debug(*v) : std::format("<bad idx {}>", idx);
    }

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

    String Disassembler::make_range(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        const u8 raw = read_u8();
        return join_line(op_name, std::format("{:02X}  ; flags=0x{:02X}", to_u32(raw), to_u32(raw)));
    }

    String Disassembler::const_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 idx = read_u16();
        return join_line(op_name, std::format("{:04X}  ; {}", idx, format_constant(idx)));
    }

    String Disassembler::jump_forward(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 off    = read_u16();
        const i64 target = static_cast<i64>(offset_) + off;
        return join_line(op_name, std::format("{:04X} -> {:04X}", off, static_cast<u32>(target)));
    }

    String Disassembler::jump_back(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 off    = read_u16();
        const i64 target = static_cast<i64>(offset_) - off;
        return join_line(op_name, std::format("{:04X} <- {:04X}", off, static_cast<u32>(target)));
    }

    // IMPORT 只带 path 索引; 绑定由后续指令完成, 无 alias 操作数。
    String Disassembler::import_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16    path_idx = read_u16();
        const String path     = format_constant(path_idx);
        return join_line(op_name, std::format("{:04X}  ; {}", path_idx, path));
    }

    // 反汇编单条指令, 返回不含偏移前缀/换行的文本, 推进 offset_ 越过该指令。
    String Disassembler::dis_instruction() {
        const u8 byte = codeunit_->code[offset_++];

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

    String Disassembler::disassemble() {
        offset_    = 0;
        String out = std::format("== {} ==\n", name_);

        // 常量池小节: 供解读各指令的常量索引操作数。
        if (!codeunit_->constants.empty()) {
            out += "\nconstants:\n";
            for (usize i = 0; i < codeunit_->constants.size(); ++i) {
                out += std::format("  {:04X}: {}\n", static_cast<u32>(i), format_value_debug(codeunit_->constants[i]));
            }
        }

        // try records 小节: 记录按 begin 升序, 与 code 段偏移对照阅读。
        if (const auto& records = codeunit_->try_records; !records.empty()) {
            out += "\ntry records:\n";
            for (usize i = 0; i < records.size(); ++i) {
                const auto& [begin, end, handle, stack_depth] = records[i];
                out += std::format("  {:04X}: [{:04X}, {:04X}) handle={:04X} stack_depth={}\n", static_cast<u32>(i),
                                   begin, end, handle, stack_depth);
            }
        }

        // code 段: 每行 偏移 + 行号(与上行同号用 '|' 占位) + 指令文本。
        // 先存 ip 再解码 -- 避免 format 实参求值顺序干扰 offset_。
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
        // name 传空即可 -- 仅 disassemble() 用它, 单指令路径不解码表头。
        Disassembler d{codeunit, {}};
        d.offset_ = offset;
        return d.dis_instruction();
    }

} // namespace aria
