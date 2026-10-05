#ifndef ARIA_STRING_ARENA_HPP
#define ARIA_STRING_ARENA_HPP

#include "common.hpp"

namespace aria {

    // 字符串字面量解析内容的落地缓冲：Lexer 把转义解码后的字节追加进来，段 token 持入其中的视图。
    // 构造记下内容上界（= 原文总长：转义解析永不膨胀输出，2 源字节 -> 1、\u{...} 不超原文），首次写入
    // 按上界一次预留，此后容量恒足、缓冲地址不再变化——token 持入的视图依赖这一点；无字符串的源一次
    // 写入都不发生。底层取 List<char> 而非 String：缓冲须跨 move 地址不变（Lexer -> TokenStream），
    // SSO 内联字节随 move 搬家会悬空视图。
    class StringArena {
    public:
        StringArena() noexcept : bound_{0} {}

        explicit StringArena(const usize content_bound) noexcept : bound_{content_bound} {}

        // 当前写入位（view_from 以此取回此后写入的段视图）。
        [[nodiscard]]
        usize mark() const noexcept {
            return buffer_.size();
        }

        // 自 mark 至当前写入位的段视图；空段返回默认空视图（未触碰缓冲的 data() 为 nullptr，不流向下游）。
        [[nodiscard]]
        StringView view_from(const usize mark) const noexcept {
            if (mark == buffer_.size()) {
                return {};
            }
            ASSERT(mark < buffer_.size(), "mark beyond arena write position");
            return StringView{buffer_.data() + mark, buffer_.size() - mark};
        }

        void append(const char byte) {
            ensure_cap();
            buffer_.push_back(byte);
        }

        void append(const StringView bytes) {
            ensure_cap();
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        }

    private:
        // 首次写入按上界一次预留；内容恒不超上界，此后容量恒足、地址不再变化。
        void ensure_cap() {
            if (buffer_.empty()) {
                ASSERT(bound_ > 0, "append into arena without content bound");
                buffer_.reserve(bound_);
            }
        }

        List<char> buffer_;
        usize      bound_;
    };

} // namespace aria

#endif // ARIA_STRING_ARENA_HPP
