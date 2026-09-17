#include "CodeUnit.hpp"

#include "bytecode/Disassembler.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {
        // 编码上限(源自 CodeUnit.hpp 的位宽事实源, 值即上限位置, 越界判定直接与上限比较)。
        constexpr u32 kMaxPopChunk       = kU8OperandMax;
        constexpr u32 kMaxShortLocalSlot = kU8OperandMax;
        constexpr u32 kMaxJumpOffset     = kU16OperandMax;
        constexpr u32 kMaxConstantIndex  = kU16OperandMax;
    } // namespace

    CodeUnit::CodeUnit(GC* gc) noexcept : code{gc}, constants{gc}, lines{gc}, try_records{gc} {}

    // ---- emit ----

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

    // ---- 字节码编码 ----

    void CodeUnit::emit_pop_n(const u32 count, const u32 line) {
        for (u32 remaining = count; remaining > 0;) {
            const u8 chunk = remaining > kMaxPopChunk ? kMaxPopChunk : static_cast<u8>(remaining);
            if (chunk == 1) { // 降级 POP(1B,免操作数)

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
        const u32 src_off = size(); // 跳转源: 占位偏移, 供 patch_jump 回填
        emit_word(0, line);         // 占位
        return src_off;
    }

    bool CodeUnit::patch_jump(const u32 src_off) {
        const u32 base_off = src_off + 2; // 偏移基准: 读完 u16 操作数后的 ip
        // 跳转目标: 当前末尾
        const u32 target_off = size();
        // 前向偏移:契约是 patch 时目标已发射(target_off >= base_off)。误用于反向时 u32 回绕成
        // 巨大值,恰好被下方 kMaxJumpOffset 上界兜住返 false(等效 emit_jump_back 的显式反向预检)。
        const u32 offset = target_off - base_off;
        if (offset > kMaxJumpOffset) {
            return false; // 越界,交调用方翻译为 Error
        }
        const auto bytes  = util::split_word(static_cast<u16>(offset)); // 小端: [低字节, 高字节]
        code[src_off]     = bytes[0];
        code[src_off + 1] = bytes[1];
        return true;
    }

    bool CodeUnit::emit_jump_back(const u32 target_off, const u32 line) {
        emit_op(OpCode::JUMP_BACK, line);
        const u32 src_off  = size();
        const u32 base_off = src_off + 2; // 偏移基准: 读完 u16 操作数后的 ip
        if (base_off < target_off || base_off - target_off > kMaxJumpOffset) {
            emit_word(0, line); // 占位,保持 code 长度一致
            return false;       // 反向或越界
        }
        const u32 offset = base_off - target_off; // 后向偏移
        emit_word(static_cast<u16>(offset), line);
        return true;
    }

    void CodeUnit::emit_load_local(const u16 slot, const u32 line) {
        if (slot <= kMaxShortLocalSlot) {
            emit_op(OpCode::LOAD_LOCAL, line);
            emit_byte(static_cast<u8>(slot), line);
        } else {
            emit_op(OpCode::LOAD_LOCAL_L, line);
            emit_word(slot, line);
        }
    }

    void CodeUnit::emit_store_local(const u16 slot, const u32 line) {
        if (slot <= kMaxShortLocalSlot) {
            emit_op(OpCode::STORE_LOCAL, line);
            emit_byte(static_cast<u8>(slot), line);
        } else {
            emit_op(OpCode::STORE_LOCAL_L, line);
            emit_word(slot, line);
        }
    }

    // ---- 常量池 ----

    u16 CodeUnit::add_constant(const Value value) {
        ASSERT(constants.size() <= kMaxConstantIndex, "constant pool overflow (>65535 constants)");
        const auto idx = static_cast<u16>(constants.size());
        constants.push(value);
        return idx;
    }

    // ---- 行号 ----

    u32 CodeUnit::line_for_offset(const u32 offset) const noexcept {
        // RLE 二分: 找最大的 entry.offset <= offset, 返回其 line。
        // 表按 offset 单调(只追加), 用 upper_bound 取首个 offset > target 的位置, 其前一条即答案。
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
            return 0; // offset 在首条之前(不应发生: 首次 emit 在 offset 0 记一条)
        }
        return lines[low - 1].line;
    }

    // ---- 异常记录表 ----

    Opt<const TryRecord*> CodeUnit::find_try_handler(const u32 ip) const noexcept {
        // 记录按 begin 单调; 二分找最后一个 begin <= ip, 向前找第一个 end > ip(最内层覆盖)。
        // 前提:try 区间良嵌套(任意两条记录不交叉重叠),交叉时"前溯第一个 end > ip"可能命中
        // 错误 handler;该不变式由编译器 try 的「入口预插占位 + 结尾回填」发射顺序保证。
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
        // low = 首个 begin > ip 的位置; 从 low-1 向前找第一个 end > ip
        while (low > 0) {
            --low;
            if (try_records[low].end > ip) {
                return &try_records[low];
            }
        }
        return std::nullopt;
    }

    // ---- 反汇编 ----

    String CodeUnit::disassemble(const StringView name) const { return Disassembler::disassembleCodeUnit(this, name); }

    // ---- 私有 ----

    void CodeUnit::record_line_(const u32 line) noexcept {
        if (!lines.empty() && lines.top().line == line) {
            return; // 同行: RLE 覆盖, 不追加
        }
        lines.push({.offset = size(), .line = line});
    }

} // namespace aria
