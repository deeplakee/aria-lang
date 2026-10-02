#ifndef ARIA_UTF8_HPP
#define ARIA_UTF8_HPP

#include "common.hpp"

namespace aria::utf8 {
    // Unicode 码点
    using codepoint = u32;

    // 替换码点 U+FFFD。
    inline constexpr codepoint kReplacementChar = 0xFFFD;

    // 该字节是否为 ASCII(0xxxxxxx);char 重载先窄化为 u8(char 可能带符号)。
    [[nodiscard]]
    constexpr bool is_ascii(const u8 byte) noexcept {
        return byte < 0x80;
    }

    [[nodiscard]]
    constexpr bool is_ascii(const char ch) noexcept {
        return is_ascii(static_cast<u8>(ch));
    }

    namespace detail {
        // 该字节是否为 UTF-8 序列的起始字节（ASCII 或多字节首字节）
        [[nodiscard]]
        constexpr bool is_lead_byte(const u8 byte) noexcept {
            return (byte & 0xC0) != 0x80;
        }

        // 该字节是否为 UTF-8 续接字节（10xxxxxx）
        [[nodiscard]]
        constexpr bool is_cont_byte(const u8 byte) noexcept {
            return (byte & 0xC0) == 0x80;
        }

        // 根据起始字节推断序列应有的字节长度；0 表示非法起始字节
        [[nodiscard]]
        constexpr u8 seq_len_from_lead(const u8 byte) noexcept {
            if (byte < 0x80)
                return 1;
            if (byte < 0xC0)
                return 0;
            if (byte < 0xE0)
                return 2;
            if (byte < 0xF0)
                return 3;
            if (byte < 0xF8)
                return 4;
            return 0;
        }

        [[nodiscard]]
        constexpr u8 cont_bits(const u8 byte) noexcept {
            return static_cast<u8>(byte & 0x3F);
        }

        // 非 ASCII 慢路径:解码 str[offset] 处的多字节序列(lead 为首字节,已由 decode_one 判定 >= 0x80)。
        // ARIA_NOINLINE:调用点绝大多数走 ASCII 快路径,组装+双重校验是冷路径,移出内联控制热路径体积。
        [[nodiscard]] ARIA_NOINLINE constexpr Pair<codepoint, u8>
        decode_multibyte(const StringView str, const usize offset, const u8 lead) noexcept {
            const u8 need = seq_len_from_lead(lead);
            if (need == 0 || offset + need > str.size()) {
                return {kReplacementChar, 1};
            }

            for (u8 i = 1; i < need; ++i) {
                if (!is_cont_byte(static_cast<u8>(str[offset + i]))) {
                    return {kReplacementChar, 1};
                }
            }

            codepoint cp = 0;
            switch (need) {
                case 2:
                    cp = static_cast<codepoint>(lead & 0x1F) << 6 | cont_bits(static_cast<u8>(str[offset + 1]));
                    break;
                case 3:
                    cp = (static_cast<codepoint>(lead & 0x0F) << 12) |
                         (static_cast<codepoint>(cont_bits(static_cast<u8>(str[offset + 1]))) << 6) |
                         cont_bits(static_cast<u8>(str[offset + 2]));
                    break;
                case 4:
                    cp = (static_cast<codepoint>(lead & 0x07) << 18) |
                         (static_cast<codepoint>(cont_bits(static_cast<u8>(str[offset + 1]))) << 12) |
                         (static_cast<codepoint>(cont_bits(static_cast<u8>(str[offset + 2]))) << 6) |
                         cont_bits(static_cast<u8>(str[offset + 3]));
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
    } // namespace detail


    // 解码 str[offset] 处的一个 UTF-8 序列,返回 {码点, 消费字节数};非法字节返回 {kReplacementChar, 1}
    // (只吞一个坏字节,便于继续扫描);offset >= str.size() 返回 {kReplacementChar, 0},不进行任何读取。
    [[nodiscard]]
    constexpr Pair<codepoint, u8> decode_one(const StringView str, const usize offset = 0) noexcept {
        if (offset >= str.size()) {
            return {kReplacementChar, 0};
        }
        const u8 lead = static_cast<u8>(str[offset]);
        if (is_ascii(lead)) {
            return {static_cast<codepoint>(lead), 1};
        }
        return detail::decode_multibyte(str, offset, lead);
    }

    // 校验 str 是否为合法的 UTF-8 文本（无非法字节、无超长编码、无代理区码点）
    [[nodiscard]]
    constexpr bool is_valid(const StringView str) noexcept {
        for (usize i = 0; i < str.size();) {
            const u8 lead = static_cast<u8>(str[i]);
            if (is_ascii(lead)) {
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
            // 复用 decode_one 的码点校验:非法时它返回 {kReplacementChar, 1},长度与 need 不等即判非法。
            const auto [cp, n] = decode_one(str, i);
            if (n != need) {
                return false;
            }
            (void) cp;
            i += need;
        }
        return true;
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


    // 码点分类工具

    // 是否 Unicode 字母(Lu/Ll/Lt/Lm/Lo);ASCII 精确判定,其余为分配区间近似(不引 Unicode 数据表)。
    [[nodiscard]]
    constexpr bool is_alpha(const codepoint cp) noexcept {
        if (cp < 0x80) {
            return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
        }
        return (cp >= 0x00C0 && cp <= 0x024F)     // 拉丁扩展
               || (cp >= 0x0370 && cp <= 0x03FF)  // 希腊
               || (cp >= 0x0400 && cp <= 0x04FF)  // 西里尔
               || (cp >= 0x4E00 && cp <= 0x9FFF)  // CJK 统一表意
               || (cp >= 0xAC00 && cp <= 0xD7A3); // 韩文音节
    }

    // 是否 Unicode 十进制数字(Nd);ASCII 精确,其余近似。
    [[nodiscard]]
    constexpr bool is_digit(const codepoint cp) noexcept {
        if (cp < 0x80) {
            return cp >= '0' && cp <= '9';
        }
        return (cp >= 0x0660 && cp <= 0x0669)     // 阿拉伯-印度
               || (cp >= 0xFF10 && cp <= 0xFF19); // 全角数字
    }

    // 是否可用于标识符起始:下划线或 Unicode 字母。
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
        return (cp >= 0x1680 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
    }
} // namespace aria::utf8

#endif // ARIA_UTF8_HPP
