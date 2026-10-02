#include "CodeUnit.hpp"

#include "bytecode/Disassembler.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {
        constexpr u32 kMaxPopChunk      = kU8OperandMax;
        constexpr u32 kMaxNLocalSlot    = 8;
        constexpr u32 kMaxJumpOffset    = kU16OperandMax;
        constexpr u32 kMaxConstantIndex = kU16OperandMax;
    } // namespace

    CodeUnit::CodeUnit(GC* gc) noexcept : code{gc}, constants{gc}, lines{gc}, try_records{gc} {}

    void CodeUnit::emit_byte(const u8 byte, const u32 line) {
        record_line_(line);
        code.push(byte);
    }

    void CodeUnit::emit_word(const u16 word, const u32 line) {
        const auto bytes = util::split_word(word); // 小端: [低字节, 高字节]
        emit_byte(bytes[0], line);
        emit_byte(bytes[1], line);
    }

    void CodeUnit::emit_op(const OpCode op, const u32 line) { emit_byte(static_cast<u8>(op), line); }

    void CodeUnit::emit_pop_n(const u32 count, const u32 line) {
        for (u32 remaining = count; remaining > 0;) {
            const u8 chunk = remaining > kMaxPopChunk ? kMaxPopChunk : static_cast<u8>(remaining);
            if (chunk == 1) {
                emit_op(OpCode::POP, line);
            } else {
                emit_op(OpCode::POP_N, line);
                emit_byte(chunk, line);
            }
            remaining -= chunk;
        }
    }

    u32 CodeUnit::emit_jump(const OpCode op, const u32 line) {
        emit_op(op, line);
        const u32 src_off = size();
        emit_word(0, line); // 占位
        return src_off;
    }

    bool CodeUnit::patch_jump(const u32 src_off) {
        const u32 base_off   = src_off + 2;
        const u32 target_off = size();
        // patch 时目标必已发射(target_off >= base_off); 误用于反向时 u32 回绕成巨值,
        // 恰被下方 kMaxJumpOffset 上界兜住返 false。
        const u32 offset = target_off - base_off;
        if (offset > kMaxJumpOffset) {
            return false;
        }
        const auto bytes  = util::split_word(static_cast<u16>(offset)); // 小端: [低字节, 高字节]
        code[src_off]     = bytes[0];
        code[src_off + 1] = bytes[1];
        return true;
    }

    bool CodeUnit::emit_jump_back(const u32 target_off, const u32 line) {
        emit_op(OpCode::JUMP_BACK, line);
        const u32 src_off  = size();
        const u32 base_off = src_off + 2;
        if (base_off < target_off || base_off - target_off > kMaxJumpOffset) {
            emit_word(0, line); // 占位,保持 code 长度一致
            return false;
        }
        const u32 offset = base_off - target_off;
        emit_word(static_cast<u16>(offset), line);
        return true;
    }

    void CodeUnit::emit_load_local(const u16 slot, const u32 line) {
        if (slot >= 1 && slot <= kMaxNLocalSlot) {
            emit_op(static_cast<OpCode>(static_cast<u8>(OpCode::LOAD_LOCAL_1) + (slot - 1)), line);
        } else {
            emit_op(OpCode::LOAD_LOCAL, line);
            emit_word(slot, line);
        }
    }

    void CodeUnit::emit_store_local(const u16 slot, const u32 line) {
        if (slot >= 1 && slot <= kMaxNLocalSlot) {
            emit_op(static_cast<OpCode>(static_cast<u8>(OpCode::STORE_LOCAL_1) + (slot - 1)), line);
        } else {
            emit_op(OpCode::STORE_LOCAL, line);
            emit_word(slot, line);
        }
    }

    u16 CodeUnit::add_constant(const Value value) {
        ASSERT(constants.size() <= kMaxConstantIndex, "constant pool overflow (>65535 constants)");
        const auto idx = static_cast<u16>(constants.size());
        constants.push(value);
        return idx;
    }

    u32 CodeUnit::line_for_offset(const u32 offset) const noexcept {
        // 表按 offset 单调(只追加); 取首个 entry.offset > offset 的位置, 其前一条即答案。
        if (lines.empty()) {
            return 0;
        }
        usize low  = 0;
        usize high = lines.size();
        while (low < high) {
            const usize mid = low + (high - low) / 2;
            if (lines[mid].offset <= offset) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        if (low == 0) {
            return 0; // 先于首条 entry; 正常不可达(首次 emit 即在 offset 0 记一条)
        }
        return lines[low - 1].line;
    }

    Opt<const TryRecord*> CodeUnit::find_try_handler(const u32 ip) const noexcept {
        // 二分找最后一个 begin <= ip, 再前溯找第一个 end > ip(最内层覆盖)。
        // 前提: try 区间良嵌套(不交叉重叠); 交叉时前溯可能命中错误 handler。
        // 良嵌套由编译器 try 的发射顺序(入口预插占位 + 结尾回填)保证。
        if (try_records.empty()) {
            return std::nullopt;
        }
        usize low  = 0;
        usize high = try_records.size();
        while (low < high) {
            const usize mid = low + (high - low) / 2;
            if (try_records[mid].begin <= ip) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        while (low > 0) {
            --low;
            if (try_records[low].end > ip) {
                return &try_records[low];
            }
        }
        return std::nullopt;
    }

    String CodeUnit::disassemble(const StringView name) const { return Disassembler::disassembleCodeUnit(this, name); }

    void CodeUnit::record_line_(const u32 line) noexcept {
        if (!lines.empty() && lines.top().line == line) {
            return;
        }
        lines.push({.offset = size(), .line = line});
    }

} // namespace aria
