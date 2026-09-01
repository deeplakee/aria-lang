#ifndef ARIA_UTF8_HPP
#define ARIA_UTF8_HPP

#include <iterator>
#include "common.hpp"

namespace aria::utf8 {
    // Unicode 码点
    using codepoint = u32;

    // 替换码点 U+FFFD，用于替换非法的 UTF-8 序列
    inline constexpr codepoint kReplacementChar = 0xFFFD;

    // UTF-8 序列的字节长度上限
    inline constexpr usize kMaxSeqLen = 4;

    namespace detail {
        // 该字节是否为 UTF-8 序列的起始字节（ASCII 或多字节首字节）
        [[nodiscard]]
        constexpr bool is_lead_byte(const u8 b) noexcept {
            // 10xxxxxx 的续接字节返回 false，其余都是起始字节
            return (b & 0xC0) != 0x80;
        }

        // 该字节是否为 UTF-8 续接字节（10xxxxxx）
        [[nodiscard]]
        constexpr bool is_cont_byte(const u8 b) noexcept {
            return (b & 0xC0) == 0x80;
        }

        // 根据起始字节推断序列应有的字节长度；0 表示非法起始字节
        [[nodiscard]]
        constexpr u8 seq_len_from_lead(const u8 b) noexcept {
            if (b < 0x80)
                return 1; // 0xxxxxxx
            if (b < 0xC0)
                return 0; // 10xxxxxx 不能作为起始字节
            if (b < 0xE0)
                return 2; // 110xxxxx
            if (b < 0xF0)
                return 3; // 1110xxxx
            if (b < 0xF8)
                return 4; // 11110xxx
            return 0;     // 11111xxx 非法
        }

        // 续接字节的低 6 位
        [[nodiscard]]
        constexpr u8 cont_bits(const u8 b) noexcept {
            return static_cast<u8>(b & 0x3F);
        }
    } // namespace detail


    // 解码位于 str[offset] 处的一个 UTF-8 序列。
    // 返回 {码点, 消费的字节数}：合法序列返回真实码点与字节数；
    // 遇到非法字节时返回 {kReplacementChar, 1}（只吞掉一个坏字节，便于继续扫描）。
    // offset 超出范围（>= str.size()）时返回 {kReplacementChar, 0}，不进行任何读取；
    // 正常使用时调用方应保证 offset < str.size()，此时返回的字节数 >= 1。
    [[nodiscard]]
    constexpr Pair<codepoint, usize> decode_one(const StringView str, const usize offset = 0) noexcept {
        if (offset >= str.size()) {
            return {kReplacementChar, 0};
        }
        // ASCII 快速路径
        const u8 lead = static_cast<u8>(str[offset]);
        if (lead < 0x80) {
            return {static_cast<codepoint>(lead), 1};
        }

        const u8 need = detail::seq_len_from_lead(lead);
        if (need == 0 || offset + need > str.size()) {
            return {kReplacementChar, 1};
        }

        // 校验所有续接字节
        for (u8 i = 1; i < need; ++i) {
            if (!detail::is_cont_byte(static_cast<u8>(str[offset + i]))) {
                return {kReplacementChar, 1};
            }
        }

        // 组装码点
        codepoint cp = 0;
        switch (need) {
            case 2:
                cp = static_cast<codepoint>(lead & 0x1F) << 6 | detail::cont_bits(static_cast<u8>(str[offset + 1]));
                break;
            case 3:
                cp = (static_cast<codepoint>(lead & 0x0F) << 12) |
                     (static_cast<codepoint>(detail::cont_bits(static_cast<u8>(str[offset + 1]))) << 6) |
                     detail::cont_bits(static_cast<u8>(str[offset + 2]));
                break;
            case 4:
                cp = (static_cast<codepoint>(lead & 0x07) << 18) |
                     (static_cast<codepoint>(detail::cont_bits(static_cast<u8>(str[offset + 1]))) << 12) |
                     (static_cast<codepoint>(detail::cont_bits(static_cast<u8>(str[offset + 2]))) << 6) |
                     detail::cont_bits(static_cast<u8>(str[offset + 3]));
                break;
            default:
                UNREACHABLE();
        }

        // 最短编码校验：码点不得低于当前长度能表示的下界
        constexpr codepoint kMin2 = 0x80;
        constexpr codepoint kMin3 = 0x800;
        constexpr codepoint kMin4 = 0x10000;
        if ((need == 2 && cp < kMin2) || (need == 3 && cp < kMin3) || (need == 4 && cp < kMin4)) {
            return {kReplacementChar, 1};
        }

        // 超出 Unicode 范围或代理区码点非法
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return {kReplacementChar, 1};
        }

        return {cp, need};
    }

    // 解码整个字符串为码点序列（非法序列被替换为 kReplacementChar）。
    // 成功总是为 true；若需要区分是否含非法序列，使用 is_valid。
    [[nodiscard]]
    inline List<codepoint> decode(const StringView str) {
        List<codepoint> out;
        out.reserve(str.size());
        for (usize i = 0; i < str.size();) {
            const auto [cp, n] = decode_one(str, i);
            out.push_back(cp);
            i += n;
        }
        return out;
    }

    // 校验 str 是否为合法的 UTF-8 文本（无非法字节、无超长编码、无代理区码点）
    [[nodiscard]]
    constexpr bool is_valid(const StringView str) noexcept {
        for (usize i = 0; i < str.size();) {
            const u8 lead = static_cast<u8>(str[i]);
            if (lead < 0x80) {
                ++i;
                continue;
            }
            const u8 need = detail::seq_len_from_lead(lead);
            if (need == 0 || i + need > str.size()) {
                return false;
            }
            for (u8 j = 1; j < need; ++j) {
                if (!detail::is_cont_byte(static_cast<u8>(str[i + j]))) {
                    return false;
                }
            }
            // 复用 decode_one 的码点校验：非法时它会返回 kReplacementChar 且长度 1，
            // 而真实长度为 need，二者不等即可判定非法。
            const auto [cp, n] = decode_one(str, i);
            if (n != need) {
                return false;
            }
            (void) cp;
            i += need;
        }
        return true;
    }

    // 计算合法 UTF-8 文本中的码点数量（非法字节按 1 计）。
    // 等价于 decode(str).size()，但不分配。
    [[nodiscard]]
    constexpr usize count(const StringView str) noexcept {
        usize n = 0;
        for (usize i = 0; i < str.size();) {
            const u8 lead = static_cast<u8>(str[i]);
            if (lead < 0x80) {
                ++i;
                ++n;
                continue;
            }
            const u8 need = detail::seq_len_from_lead(lead);
            i += (need == 0) ? 1 : need; // 不严格校验，仅按长度推进
            ++n;
        }
        return n;
    }

    // 将一个码点编码为 UTF-8 字节串。非法码点（>0x10FFFF 或代理区）返回空串。
    [[nodiscard]]
    constexpr String encode(const codepoint cp) {
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return {};
        }

        String out;
        if (cp < 0x80) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        return out;
    }


    // 码点分类工具，供 tokenizer 判定字符类别时使用

    // 是否 ASCII 字母或数字（即 [A-Za-z0-9]）
    [[nodiscard]]
    constexpr bool is_alnum(const codepoint cp) noexcept {
        return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
    }

    // 是否 Unicode 字母（Lu/Ll/Lt/Lm/Lo）。ASCII 范围内精确判定，
    // 其余通过 Unicode 分配区间近似，避免引入庞大的 Unicode 数据表。
    [[nodiscard]]
    constexpr bool is_alpha(const codepoint cp) noexcept {
        if (cp < 0x80) {
            return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
        }
        // 近似：常见字母分配区间（拉丁扩展、希腊、西里尔、CJK 表意、假名等）
        return (cp >= 0x00C0 && cp <= 0x024F)     // 拉丁扩展
               || (cp >= 0x0370 && cp <= 0x03FF)  // 希腊
               || (cp >= 0x0400 && cp <= 0x04FF)  // 西里尔
               || (cp >= 0x4E00 && cp <= 0x9FFF)  // CJK 统一表意
               || (cp >= 0xAC00 && cp <= 0xD7A3); // 韩文音节
    }

    // 是否 Unicode 十进制数字（Nd）。ASCII 范围内精确判定，其余近似为常见数字区间。
    [[nodiscard]]
    constexpr bool is_digit(const codepoint cp) noexcept {
        if (cp < 0x80) {
            return cp >= '0' && cp <= '9';
        }
        return (cp >= 0x0660 && cp <= 0x0669)     // 阿拉伯-印度
               || (cp >= 0xFF10 && cp <= 0xFF19); // 全角数字
    }

    // 是否可用于标识符起始：下划线，或 Unicode 字母（Lu/Ll/Lt/Lm/Lo/Nl）。
    // ASCII 范围内做精确判定，其余通过 is_alpha 近似。
    [[nodiscard]]
    constexpr bool is_id_start(const codepoint cp) noexcept {
        if (cp == '_')
            return true;
        return is_alpha(cp);
    }

    // 是否可用于标识符续接字符：标识符起始字符集合并上 Unicode 数字 Nd
    [[nodiscard]]
    constexpr bool is_id_continue(const codepoint cp) noexcept {
        return is_id_start(cp) || is_digit(cp);
    }

    // 是否空白字符：ASCII 空白或 Unicode 分隔符/制表类（近似）
    [[nodiscard]]
    constexpr bool is_whitespace(const codepoint cp) noexcept {
        switch (cp) {
            case 0x09: // HT
            case 0x0A: // LF
            case 0x0D: // CR
            case 0x20: // Space
            case 0x0B: // VT
            case 0x0C: // FF
            case 0xA0: // NBSP
                return true;
            default:
                break;
        }
        // Unicode 空白/分隔符区间（近似，不含全角空格 U+3000 之外的特殊情况）
        return (cp >= 0x1680 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
    }


    // 前向码点迭代器：对一个 UTF-8 字符串视图逐码点遍历。
    // 非法字节会被当作 kReplacementChar 迭代一次（advance 一个字节）。
    class iterator {
    public:
        using value_type        = codepoint;
        using difference_type   = isize;
        using pointer           = const codepoint*;
        using reference         = codepoint;
        using iterator_category = std::forward_iterator_tag;

        constexpr iterator() noexcept : str_{}, pos_{0} {}
        constexpr explicit iterator(StringView s, usize pos = 0) noexcept : str_{s}, pos_{pos} {}

        [[nodiscard]]
        constexpr codepoint operator*() const noexcept {
            return decode_one(str_, pos_).first;
        }

        constexpr iterator& operator++() noexcept {
            const usize n = decode_one(str_, pos_).second;
            pos_ += n;
            return *this;
        }

        constexpr iterator operator++(int) noexcept { // NOLINT(cert-dcl21-cpp)
            iterator tmp = *this;
            ++(*this);
            return tmp;
        }

        [[nodiscard]]
        constexpr bool operator==(const iterator& other) const noexcept {
            return pos_ == other.pos_;
        }

        // 当前已消费到的字节偏移
        [[nodiscard]]
        constexpr usize byte_offset() const noexcept {
            return pos_;
        }

    private:
        StringView str_;
        usize      pos_;
    };

    // 码点视图：把一个 UTF-8 字符串包装为可遍历的码点区间。
    // 用法： for (auto cp : utf8::view(str)) { ... }
    class view {
    public:
        constexpr explicit view(StringView s) noexcept : str_(s) {}

        [[nodiscard]]
        constexpr iterator begin() const noexcept {
            return iterator{str_, 0};
        }

        [[nodiscard]]
        constexpr iterator end() const noexcept {
            return iterator{str_, str_.size()};
        }

        [[nodiscard]]
        constexpr StringView str() const noexcept {
            return str_;
        }

    private:
        StringView str_;
    };
} // namespace aria::utf8

#endif // ARIA_UTF8_HPP
