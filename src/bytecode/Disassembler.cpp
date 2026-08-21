#include "Disassembler.hpp"

#include <format>

#include "bytecode/CodeUnit.hpp"
#include "bytecode/code.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
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

        // 字符串转义见 util/util.hpp 的 aria::util::escape_string;字符串字面量渲染 `"..."` 见
        // value/Value.hpp 的 aria::format_string(escape_string 转义内部,外层补双引号)。

        // f64 可读化见 value/Value.hpp 的 aria::format_f64(VM PRINT 与反汇编共用,避免重复)。

        // 值可读化:nil/true/false/整数/浮点/"字符串"/<对象描述>。供常量池小节与 LOAD_CONST/CLOSURE 注释共用。
        // Obj 走虚函数 obj->to_string()(子类型按需 override,默认 <Type at 0xaddr>);ObjString 特判:
        // 反汇编里字符串字面量约定用 "..." 形式(转义后),而非 ObjString::to_string 的 '...' Python 风格。
        String dis_constant(const Value v) {
            switch (v.type()) {
                case Value::Type::Nil:
                    return "nil";
                case Value::Type::Bool:
                    return v.as_bool() ? "true" : "false";
                case Value::Type::Int:
                    return std::format("{}", v.as_int());
                case Value::Type::F64:
                    return format_f64(v.as_f64());
                case Value::Type::Obj: {
                    const Object* obj = v.as_obj();
                    if (Object::is<ObjString>(obj)) {
                        return format_string(Object::as<ObjString>(obj));
                    }
                    return obj->to_string();
                }
            }
            UNREACHABLE();
        }

        // 把 opcode 名与操作数段拼成一行:左对齐 16 列的 op_name,空操作数即裸名,末尾尾随空格裁掉。
        String join_line(StringView op_name, const StringView operands) {
            String line = std::format("{:<16}", op_name);
            if (!operands.empty()) {
                line.append("  ").append(operands);
            }
            while (!line.empty() && line.back() == ' ') {
                line.pop_back();
            }
            return line;
        }

        // 无操作数指令(HALT / LOAD_NIL / LOAD_TRUE / ... / RETURN):仅输出名字。
        String simple_instruction(StringView op_name) { return std::format("{}", op_name); }

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
        offset_ = codeunit_->code.size();
        return "<truncated>";
    }

    // 越界预检:读取 need 字节是否会越过末尾(残缺字节码将被截断)。
    bool Disassembler::is_truncated(const usize need) const noexcept { return offset_ + need > codeunit_->code.size(); }

    // 常量索引的注释渲染(名字索引与 LOAD_CONST/CLOSURE 共用):按 idx 取常量,越界退化为 <bad idx>。
    // 名字索引(全局/字段/类/方法名)的操作数本就是常量池里的 ObjString,无特殊对待,直接走 dis_constant
    // (其内部已对 ObjString 特判为 "..." 字面量形态)。只读,不推进 offset_。
    String Disassembler::format_constant(const u16 idx) const {
        const auto v = constant_at(codeunit_, idx);
        return v ? dis_constant(*v) : std::format("<bad idx {}>", idx);
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

    // 单字节 u8 操作数(LOAD_LOCAL/STORE_LOCAL/LOAD_UPVALUE/STORE_UPVALUE/POP_N/CALL):`{:02X}`。
    String Disassembler::u8_instruction(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        return join_line(op_name, std::format("{:02X}", to_u32(read_u8())));
    }

    // 双字节 u16 槽/计数(LOAD_LOCAL_L/STORE_LOCAL_L/MAKE_LIST/MAKE_MAP):`{:04X}`。
    String Disassembler::u16_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        return join_line(op_name, std::format("{:04X}", read_u16()));
    }

    // MAKE_RANGE:单字节 flags,`{:02X}` 操作数 + `flags=0x{:02X}` 注释。
    String Disassembler::make_range(const StringView op_name) {
        if (is_truncated(1)) {
            return join_line(op_name, truncated());
        }
        const u8 raw = read_u8();
        return join_line(op_name, std::format("{:02X}  ; flags=0x{:02X}", to_u32(raw), to_u32(raw)));
    }

    // 常量索引(LOAD_CONST/CLOSURE):`{:04X}` 操作数 + 常量可读化注释(越界 `<bad idx>`)。
    String Disassembler::const_instruction(const StringView op_name) {
        if (is_truncated(2)) {
            return join_line(op_name, truncated());
        }
        const u16 idx = read_u16();
        return join_line(op_name, std::format("{:04X}  ; {}", idx, format_constant(idx)));
    }

    // 名字索引(全局/字段/类/方法名):操作数即常量池索引,渲染与 const_instruction 一致,故直接委托。
    String Disassembler::name_instruction(const StringView op_name) { return const_instruction(op_name); }

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

    // IMPORT:u16 path + u16 alias,两 hex 操作数 + `path as alias` 注释(名字)。
    String Disassembler::import_instruction(const StringView op_name) {
        if (is_truncated(4)) {
            return join_line(op_name, truncated());
        }
        const u16    path_idx  = read_u16();
        const u16    alias_idx = read_u16();
        const String path      = format_constant(path_idx);
        const String alias     = format_constant(alias_idx);
        return join_line(op_name, std::format("{:04X} {:04X}  ; {} as {}", path_idx, alias_idx, path, alias));
    }

    // INVOKE_METHOD:u16 name + u8 argc,name+argc hex 操作数 + `name argc=N` 注释。
    String Disassembler::invoke_instruction(const StringView op_name) {
        if (is_truncated(3)) {
            return join_line(op_name, truncated());
        }
        const u16    name_idx = read_u16();
        const String name     = format_constant(name_idx);
        const auto   argc     = static_cast<u32>(read_u8());
        return join_line(op_name, std::format("{:04X} {:02X}  ; {} argc={}", name_idx, argc, name, argc));
    }

    // 反汇编单条指令(从 codeunit_->code[offset_] 起),返回指令文本(不含偏移前缀/换行),推进 offset_ 越过该指令。
    // 每个 opcode 一支,显式调用对应解码函数并直接传字面量名(与参考实现风格一致),不借 opcode->名查表。
    String Disassembler::dis_instruction() {
        const u8 byte = codeunit_->code[offset_++]; // opcode 字节

        // 越界 opcode 字节(无对应枚举)容错:直接报 bad opcode 并停解码。
        if (byte > static_cast<u8>(OpCode::RETURN)) {
            return std::format("<bad opcode 0x{:02X}>", to_u32(byte));
        }

        switch (static_cast<OpCode>(byte)) {
            case OpCode::HALT:
                return simple_instruction("HALT");
            case OpCode::LOAD_CONST:
                return const_instruction("LOAD_CONST");
            case OpCode::LOAD_NIL:
                return simple_instruction("LOAD_NIL");
            case OpCode::LOAD_TRUE:
                return simple_instruction("LOAD_TRUE");
            case OpCode::LOAD_FALSE:
                return simple_instruction("LOAD_FALSE");
            case OpCode::LOAD_IMM:
                return load_imm("LOAD_IMM");
            case OpCode::LOAD_LOCAL:
                return u8_instruction("LOAD_LOCAL");
            case OpCode::STORE_LOCAL:
                return u8_instruction("STORE_LOCAL");
            case OpCode::LOAD_LOCAL_L:
                return u16_instruction("LOAD_LOCAL_L");
            case OpCode::STORE_LOCAL_L:
                return u16_instruction("STORE_LOCAL_L");
            case OpCode::LOAD_UPVALUE:
                return u8_instruction("LOAD_UPVALUE");
            case OpCode::STORE_UPVALUE:
                return u8_instruction("STORE_UPVALUE");
            case OpCode::CLOSE_UPVALUE:
                return simple_instruction("CLOSE_UPVALUE");
            case OpCode::DEF_GLOBAL:
                return name_instruction("DEF_GLOBAL");
            case OpCode::LOAD_GLOBAL:
                return name_instruction("LOAD_GLOBAL");
            case OpCode::STORE_GLOBAL:
                return name_instruction("STORE_GLOBAL");
            case OpCode::LOAD_FIELD:
                return name_instruction("LOAD_FIELD");
            case OpCode::STORE_FIELD:
                return name_instruction("STORE_FIELD");
            case OpCode::LOAD_INDEX:
                return simple_instruction("LOAD_INDEX");
            case OpCode::STORE_INDEX:
                return simple_instruction("STORE_INDEX");
            case OpCode::LOAD_THIS_FIELD:
                return name_instruction("LOAD_THIS_FIELD");
            case OpCode::STORE_THIS_FIELD:
                return name_instruction("STORE_THIS_FIELD");
            case OpCode::EQUAL:
                return simple_instruction("EQUAL");
            case OpCode::NOT_EQUAL:
                return simple_instruction("NOT_EQUAL");
            case OpCode::STRICT_EQUAL:
                return simple_instruction("STRICT_EQUAL");
            case OpCode::STRICT_NOT_EQUAL:
                return simple_instruction("STRICT_NOT_EQUAL");
            case OpCode::GREATER:
                return simple_instruction("GREATER");
            case OpCode::GREATER_EQUAL:
                return simple_instruction("GREATER_EQUAL");
            case OpCode::LESS:
                return simple_instruction("LESS");
            case OpCode::LESS_EQUAL:
                return simple_instruction("LESS_EQUAL");
            case OpCode::ADD:
                return simple_instruction("ADD");
            case OpCode::SUBTRACT:
                return simple_instruction("SUBTRACT");
            case OpCode::MULTIPLY:
                return simple_instruction("MULTIPLY");
            case OpCode::DIVIDE:
                return simple_instruction("DIVIDE");
            case OpCode::MOD:
                return simple_instruction("MOD");
            case OpCode::NOT:
                return simple_instruction("NOT");
            case OpCode::NEGATE:
                return simple_instruction("NEGATE");
            case OpCode::POP:
                return simple_instruction("POP");
            case OpCode::POP_N:
                return u8_instruction("POP_N");
            case OpCode::DUP:
                return simple_instruction("DUP");
            case OpCode::DUP2:
                return simple_instruction("DUP2");
            case OpCode::PRINT:
                return simple_instruction("PRINT");
            case OpCode::NOP:
                return simple_instruction("NOP");
            case OpCode::JUMP:
                return jump_forward("JUMP");
            case OpCode::JUMP_TRUE:
                return jump_forward("JUMP_TRUE");
            case OpCode::JUMP_TRUE_OR_POP:
                return jump_forward("JUMP_TRUE_OR_POP");
            case OpCode::JUMP_FALSE:
                return jump_forward("JUMP_FALSE");
            case OpCode::JUMP_FALSE_OR_POP:
                return jump_forward("JUMP_FALSE_OR_POP");
            case OpCode::JUMP_BACK:
                return jump_back("JUMP_BACK");
            case OpCode::CALL:
                return u8_instruction("CALL");
            case OpCode::CLOSURE:
                return const_instruction("CLOSURE");
            case OpCode::LOAD_OBJECT:
                return simple_instruction("LOAD_OBJECT");
            case OpCode::MAKE_CLASS:
                return name_instruction("MAKE_CLASS");
            case OpCode::MAKE_METHOD:
                return name_instruction("MAKE_METHOD");
            case OpCode::MAKE_STATIC:
                return name_instruction("MAKE_STATIC");
            case OpCode::LOAD_SUPER_METHOD:
                return name_instruction("LOAD_SUPER_METHOD");
            case OpCode::INVOKE_METHOD:
                return invoke_instruction("INVOKE_METHOD");
            case OpCode::MAKE_LIST:
                return u16_instruction("MAKE_LIST");
            case OpCode::MAKE_MAP:
                return u16_instruction("MAKE_MAP");
            case OpCode::MAKE_RANGE:
                return make_range("MAKE_RANGE");
            case OpCode::IMPORT:
                return import_instruction("IMPORT");
            case OpCode::THROW:
                return simple_instruction("THROW");
            case OpCode::RETURN:
                return simple_instruction("RETURN");
        }
        UNREACHABLE();
    }

    Disassembler::Disassembler(const CodeUnit* codeunit, const StringView name) :
        codeunit_{codeunit}, name_{name}, offset_{0} {}

    // 反汇编整个 CodeUnit,返回带表头与逐指令文本的字符串。详见 Disassembler.hpp 的文档注释。
    String Disassembler::disassemble() {
        offset_    = 0; // 从头解码,对象可复用
        String out = std::format("== {} ==\n", name_);

        // 常量池小节(非空才列):便于解读常量索引操作数。
        if (!codeunit_->constants.empty()) {
            out += "\nconstants:\n";
            for (usize i = 0; i < codeunit_->constants.size(); ++i) {
                out += std::format("  {:04X}: {}\n", static_cast<u32>(i), dis_constant(codeunit_->constants[i]));
            }
        }

        // code 段:始终列出(即便为空),与 constants 段区分。
        // 每行:偏移(4 hex) + 行号(右对齐 4 列十进制,与上行同号用 '|' 占位) + 指令文本。
        // 先存 ip 再解码,避免 format 实参求值顺序干扰 offset_。
        out += "\ncode:\n";
        Opt<u32> prev_line = std::nullopt;
        while (offset_ < codeunit_->code.size()) {
            const usize  ip       = offset_;
            const u32    line     = codeunit_->line_for_offset(ip);
            const String line_col = prev_line == line ? String{"   |"} : std::format("{:>4}", line);
            prev_line             = line;
            out += std::format("{:04X} {} {}\n", static_cast<u32>(ip), line_col, dis_instruction());
        }
        out += "\n== end ==\n";
        return out;
    }

    String Disassembler::disassembleCodeUnit(const CodeUnit* codeunit, const StringView name) {
        return Disassembler{codeunit, name}.disassemble();
    }

} // namespace aria
