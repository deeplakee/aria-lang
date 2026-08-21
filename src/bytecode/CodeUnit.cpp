#include "CodeUnit.hpp"

#include "bytecode/Disassembler.hpp"
#include "util/util.hpp"

namespace aria {

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
            const u8 chunk = remaining > 255 ? 255 : static_cast<u8>(remaining);
            // chunk==1 时发 POP(1B) 而非 POP_N 1(2B): 省一字节 + 免读操作数, 与
            // emit_load_local/emit_store_local 的短/长变体分流同思路; POP_N 1 与 POP 栈效应等价。
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
        const usize off = size();
        emit_word(0, line); // 占位
        return off;
    }

    bool CodeUnit::patch_jump(const usize off) {
        const u32 target = size();
        const u32 diff   = target - (static_cast<u32>(off) + 2);
        if (diff > 65535) {
            return false; // 越界,交调用方翻译为 Error
        }
        code[off]     = static_cast<u8>(diff & 0xFF);
        code[off + 1] = static_cast<u8>(diff >> 8);
        return true;
    }

    bool CodeUnit::emit_jump_back(const u32 target, const u32 line) {
        emit_op(OpCode::JUMP_BACK, line);
        const u32 off   = size();
        const u32 after = off + 2; // ip 读完操作数后
        if (after < target || after - target > 65535) {
            emit_word(0, line); // 占位,保持 code 长度一致
            return false;
        }
        emit_word(static_cast<u16>(after - target), line);
        return true;
    }

    void CodeUnit::emit_load_local(const u16 slot, const u32 line) {
        if (slot < 256) {
            emit_op(OpCode::LOAD_LOCAL, line);
            emit_byte(static_cast<u8>(slot), line);
        } else {
            emit_op(OpCode::LOAD_LOCAL_L, line);
            emit_word(slot, line);
        }
    }

    void CodeUnit::emit_store_local(const u16 slot, const u32 line) {
        if (slot < 256) {
            emit_op(OpCode::STORE_LOCAL, line);
            emit_byte(static_cast<u8>(slot), line);
        } else {
            emit_op(OpCode::STORE_LOCAL_L, line);
            emit_word(slot, line);
        }
    }

    // ---- 常量池 ----

    u16 CodeUnit::add_constant(const Value value) {
        ASSERT(constants.size() < 65536, "constant pool overflow (>65535 constants)");
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
        usize lo = 0;
        usize hi = lines.size();
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (static_cast<usize>(lines[mid].offset) <= offset) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        if (lo == 0) {
            return 0; // offset 在首条之前(不应发生: 首次 emit 在 offset 0 记一条)
        }
        return lines[lo - 1].line;
    }

    // ---- 异常记录表 ----

    Opt<u32> CodeUnit::find_try_handler(const u32 ip) const noexcept {
        // 记录按 begin 单调; 二分找最后一个 begin <= ip, 向前找第一个 end > ip(最内层覆盖)。
        if (try_records.empty()) {
            return std::nullopt;
        }
        usize lo = 0;
        usize hi = try_records.size();
        while (lo < hi) {
            const usize mid = lo + (hi - lo) / 2;
            if (try_records[mid].begin <= ip) {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
        // lo = 首个 begin > ip 的位置; 从 lo-1 向前找第一个 end > ip
        while (lo > 0) {
            --lo;
            if (try_records[lo].end > ip) {
                return try_records[lo].handle;
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
