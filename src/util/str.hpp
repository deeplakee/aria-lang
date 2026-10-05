#ifndef ARIA_STR_HPP
#define ARIA_STR_HPP

#include <cstring>

#include "common.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"

namespace aria::str {

    // \u{...} 转义的失败类别:词法期按类别报 InvalidEscape(锚点 = '\' 起点),展开侧输入必合法、
    // 各类别不可达。
    enum class UnicodeEscapeError : u8 {
        MissingOpenBrace,    // 'u' 后缺 '{'
        MissingCloseBrace,   // hex 段后缺 '}'
        BadHexDigits,        // 花括号内含非十六进制文本
        CodepointOutOfRange, // 码点越界(> 0x10FFFF 或代理区)
    };

    // 解析 \u{hex} 转义:raw 为从 'u' 起的源文余串。成功返回 {解码字节串(1-4 字节 UTF-8), 消费的
    // 原始字节数(含 'u' 与两侧花括号)};失败返回类别,由调用方按各自通道处置(词法期报错 / 展开
    // 侧断言)。词法期校验与内容展开两侧的唯一解析口。
    [[nodiscard]]
    inline Result<Pair<String, usize>, UnicodeEscapeError> decode_unicode_escape(const StringView raw) {
        usize pos = 1; // 消费 'u'
        if (pos >= raw.size() || raw[pos] != '{') {
            return std::unexpected(UnicodeEscapeError::MissingOpenBrace);
        }
        ++pos; // 消费 '{'
        const usize digits_begin = pos;
        while (pos < raw.size() && raw[pos] != '}') {
            ++pos;
        }
        if (pos >= raw.size()) {
            return std::unexpected(UnicodeEscapeError::MissingCloseBrace);
        }
        const auto parsed = util::try_parse<i64>(raw.substr(digits_begin, pos - digits_begin), 16);
        if (!parsed) {
            return std::unexpected(UnicodeEscapeError::BadHexDigits);
        }
        const auto                cp            = static_cast<utf8::codepoint>(*parsed);
        constexpr utf8::codepoint kMaxCodepoint = 0x10FFFF;
        constexpr utf8::codepoint kSurrogateLo  = 0xD800;
        constexpr utf8::codepoint kSurrogateHi  = 0xDFFF;
        if (cp > kMaxCodepoint || (cp >= kSurrogateLo && cp <= kSurrogateHi)) {
            return std::unexpected(UnicodeEscapeError::CodepointOutOfRange);
        }
        return Pair{utf8::encode(cp), pos + 1};
    }

    // 展开 raw 中的转义序列,直写 out 并返回写入字节数。raw 为字符串字面量的内层原文(转义未展开,
    // 含转义源文本);调用方按展开后长度备好 out 容量(词法期扫描已记账 decoded_len)。词法期已校验
    // 全部转义合法,故本函数无错误路径,未知转义序列经 UNREACHABLE 拦截(防词法/展开两侧转义集合
    // 漂移)。'\\' 后随字节不存在串尾悬空形态(词法期报 UnterminatedString)。
    [[nodiscard]]
    inline usize decode_string_content(const StringView raw, char* out) {
        char*       cursor = out;
        usize       pos    = 0;
        const usize size   = raw.size();
        while (pos < size) {
            if (raw[pos] != '\\') {
                // 普通段整块拷贝(到下一个 '\\' 或末尾),含多字节 UTF-8 透传
                const usize begin = pos;
                while (pos < size && raw[pos] != '\\') {
                    ++pos;
                }
                const usize run = pos - begin;
                std::memcpy(cursor, raw.data() + begin, run);
                cursor += run;
                continue;
            }
            ++pos; // 消费 '\'
            switch (raw[pos]) {
                case '"':
                    *cursor++ = '"';
                    ++pos;
                    break;
                case '\'':
                    *cursor++ = '\'';
                    ++pos;
                    break;
                case '\\':
                    *cursor++ = '\\';
                    ++pos;
                    break;
                case 'n':
                    *cursor++ = '\n';
                    ++pos;
                    break;
                case 't':
                    *cursor++ = '\t';
                    ++pos;
                    break;
                case 'r':
                    *cursor++ = '\r';
                    ++pos;
                    break;
                case '0':
                    *cursor++ = '\0';
                    ++pos;
                    break;
                case '$':
                    *cursor++ = '$';
                    ++pos;
                    break;
                case 'u': {
                    // \u{hex}:词法期已校验,失败不可达,ASSERT 兜底(防词法/展开两侧转义集合漂移);
                    // raw[pos] == 'u',余串从 'u' 起传入,consumed 含 'u' 与两侧花括号
                    const auto parsed = decode_unicode_escape(raw.substr(pos));
                    ASSERT(parsed.has_value(), "invalid \\u escape after lex-time validation");
                    const auto& [encoded, consumed] = *parsed;
                    std::memcpy(cursor, encoded.data(), encoded.size());
                    cursor += encoded.size();
                    pos += consumed;
                    break;
                }
                default:
                    UNREACHABLE();
            }
        }
        return static_cast<usize>(cursor - out);
    }

} // namespace aria::str

#endif // ARIA_STR_HPP
