#ifndef ARIA_STRING_ARENA_HPP
#define ARIA_STRING_ARENA_HPP

#include <algorithm>

#include "common.hpp"

namespace aria {

    // 字符串字面量解析内容的落地缓冲：Lexer 在段协议（open_segment -> append -> close_segment）下
    // 把转义解码后的字节追加进来，段 token 持入 close_segment 发放的整段视图（同一时刻至多一段
    // 开着，Lexer 的扫描形态天然如此）。存储按实际内容分块增长：首块容量 min(上界, kMaxFirstChunk)
    // ——上界 = 原文总长，转义解析永不膨胀输出，故任何分配不超证明所需；当前块写满则整块冻结收进
    // frozens_（块地址不动，已发视图恒稳），开着的那段尚无视图、拷入新块头部续写。无字符串的源一次
    // 写入都不发生。底层取 List<char> 而非 String：块须跨 move 地址不变（Lexer -> TokenStream、冻结
    // 收编皆是 move 指针不搬字节），SSO 内联字节随 move 搬家会悬空视图。
    class StringArena {
    public:
        StringArena() noexcept : budget_{0}, open_mark_{0} {}

        explicit StringArena(const usize content_bound) noexcept : budget_{content_bound}, open_mark_{0} {}

        // 起一段：此后 append 的字节属该段，直至 close_segment 发放视图。
        void open_segment() { open_mark_ = buffer_.size(); }

        // 发放 [段起点, 当前写入位) 的整段连续视图；空段返回默认空视图（未触碰缓冲，data() 不流向下游）。
        [[nodiscard]]
        StringView close_segment() const {
            if (open_mark_ == buffer_.size()) {
                return {};
            }
            return StringView{buffer_.data() + open_mark_, buffer_.size() - open_mark_};
        }

        void append(const char byte) {
            ensure_cap(1);
            buffer_.push_back(byte);
        }

        void append(const StringView bytes) {
            ensure_cap(bytes.size());
            buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
        }

    private:
        // 首块容量上限：上界不超它的源一次预留到位、零搬移（真实代码文件全覆盖）；更大的源走分块
        // 增长，分配随实际内容走、不再按整源预留。
        static constexpr usize kMaxFirstChunk = 4 * 1024 * 1024;

        // 容量不足则增块；n = 本次待写字节数。
        void ensure_cap(const usize n) {
            if (buffer_.size() + n <= buffer_.capacity()) {
                return;
            }
            ASSERT(budget_ > 0, "append into arena without content bound");
            if (buffer_.capacity() == 0) {
                buffer_.reserve(std::min(budget_, std::max(kMaxFirstChunk, n)));
                return;
            }
            freeze_and_grow(n);
        }

        // 当前块写满：闭段前缀随块长存（视图恒稳），开段尾尚无视图、拷入新块头部；新块容量钳在
        // 剩余上界内，恒容得下「开段长 + 本次写入」。
        void freeze_and_grow(const usize n) {
            budget_ -= open_mark_; // 冻结块的闭段前缀转为已消耗，从剩余额度扣除
            const auto& frozen = freeze();
            buffer_.reserve(std::min(budget_, std::max(2 * frozen.capacity(), frozen.size() - open_mark_ + n)));
            buffer_.insert(buffer_.end(), frozen.data() + open_mark_, frozen.data() + frozen.size());
            open_mark_ = 0;
        }

        const List<char>& freeze() {
            frozens_.push_back(std::move(buffer_));
            return frozens_.back();
        }

        List<char>       buffer_;    // 当前写块（容量手动管：交 List 自动倍增会整体搬移、悬空已发视图）
        List<List<char>> frozens_;   // 过往块（只读长存，闭段视图所指）
        usize            budget_;    // 剩余可分配量 = 内容上界 - 冻结块存活内容；任何一块的容量恒不超它
        usize            open_mark_; // 当前段在当前块内的起点
    };

} // namespace aria

#endif // ARIA_STRING_ARENA_HPP
