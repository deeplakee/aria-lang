#include "CodeUnit.hpp"

#include "bytecode/Disassembler.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {
        // 操作数位宽决定的编码上限(u8 操作数最大 255,u16 操作数最大 65535):
        //   kMaxPopChunk       -- POP_N 单块最多 255(操作数 u8);
        //   kMaxShortLocalSlot -- 局部槽短变体(LOAD/STORE_LOCAL + u8)最大槽号 255,超过则用长变体(_L + u16);
        //   kMaxJumpOffset     -- 跳转偏移最大 65535(操作数 u16,前向 patch/后向 jump_back 共用);
        //   kMaxConstantCount  -- 常量池最多 65536 项(u16 索引 0..65535).
        // 集中定义,使各编码点共享同一来源,无散落魔数。
        constexpr u32 kMaxPopChunk       = 255;
        constexpr u32 kMaxShortLocalSlot = 255;
        constexpr u32 kMaxJumpOffset     = 65535;
        constexpr u32 kMaxConstantCount  = 65536;
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
            if (chunk == 1) {
                emit_op(OpCode::POP, line);
            } else {
                emit_op(OpCode::POP_N, line);
                emit_byte(chunk, line);
            }
            remaining -= chunk;
        }
    }

    usize CodeUnit::emit_jump(const OpCode op, const u32 line) {
        emit_op(op, line);
        const usize src_off = size(); // 跳转源: 占位偏移, 供 patch_jump 回填
        emit_word(0, line);           // 占位
        return src_off;
    }

    bool CodeUnit::patch_jump(const usize src_off) {
        const u32 base_off   = static_cast<u32>(src_off) + 2; // 偏移基准: 读完 u16 操作数后的 ip
        const u32 target_off = size();                        // 跳转目标: 当前末尾
        const u32 offset     = target_off - base_off;        // 前向偏移
        if (offset > kMaxJumpOffset) {
            return false; // 越界,交调用方翻译为 Error
        }
        const auto bytes = util::split_word(static_cast<u16>(offset)); // 小端: [低字节, 高字节]
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
        ASSERT(constants.size() < kMaxConstantCount, "constant pool overflow (>65535 constants)");
        const auto idx = static_cast<u16>(constants.size());
        constants.push(value);
        return idx;
    }

    // ---- 行号 ----

    u32 CodeUnit::line_for_offset(const usize offset) const noexcept {
        // RLE 二分: 找最大的 entry.offset <= offset, 返回其 line。
        // 表按 offset 单调(只追加), 用 upper_bound 取首个 offset > target 的位置, 其前一条即答案。
        if (lines.empty()) {
            return 0;
        }
        usize low = 0;
        usize high = lines.size();
        while (low < high) {
            const usize mid = low + (high - low) / 2;
            if (static_cast<usize>(lines[mid].offset) <= offset) {
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

    Opt<u32> CodeUnit::find_try_handler(const u32 ip) const noexcept {
        // 记录按 begin 单调; 二分找最后一个 begin <= ip, 向前找第一个 end > ip(最内层覆盖)。
        if (try_records.empty()) {
            return std::nullopt;
        }
        usize low = 0;
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
                return try_records[low].handle;
            }
        }
        return std::nullopt;
    }

    // ---- 反汇编 ----

    String CodeUnit::disassemble(const StringView name) const { return Disassembler::disassembleCodeUnit(this, name); }

    // ---- 私有 ----

    void CodeUnit::record_line_(const u32 line) noexcept {
        const usize offset = code.size();
        if (!lines.empty() && lines.top().line == line) {
            return; // 同行: RLE 覆盖, 不追加
        }
        lines.push({.offset = static_cast<u32>(offset), .line = line});
    }

} // namespace aria
