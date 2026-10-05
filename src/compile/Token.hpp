#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <type_traits>
#include <variant>
#include "common.hpp"
#include "compile/StringArena.hpp"
#include "compile/TokenType.hpp"
#include "util/source_file.hpp"

namespace aria {
    using src::SourceLoc;

    // 字面量 token 携带的解析值；Integer 以 i64 容纳（i48 值域，越界在语义阶段拒绝），字符串为已解析
    // 转义内容的视图（指向 TokenStream::strings）。平凡可拷贝是 token 表零搬移与读侧单缓存行的前提。
    using TokenValue = std::variant<std::monostate, i64, f64, StringView>;

    // 词法单元：lexer 产出的最小语法单位。lexeme 不落指针，只存字节数，读时按 loc 自源缓冲重建
    // （词法期各 token 的 lexeme 区间恒以 loc 的偏移为起点）；字符串值借用 TokenStream::strings。
    // 借出的视图均不得越过所引对象的存活期。
    class Token {
    public:
        Token() noexcept : Token{TokenType::Eof, {}, {}, {}} {}

        Token(const TokenType type, const StringView lexeme, const SourceLoc loc) noexcept :
            Token{type, lexeme, loc, {}} {}

        [[nodiscard]]
        static Token make_integer(const i64 value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::Integer, lexeme, loc, value};
        }

        [[nodiscard]]
        static Token make_float(const f64 value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::Float, lexeme, loc, value};
        }

        // value 为已解析转义后的字符串内容（视图指向 TokenStream::strings，须在其存活期内使用）；
        // lexeme 保留原始源码文本。
        [[nodiscard]]
        static Token make_string(StringView value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::String, lexeme, loc, value};
        }

        // 插值串字面段三厂：value 为该段已解析转义后的内容（视图指向 TokenStream::strings），lexeme 保留
        // 该段原文（不含边界 ${ 与 }，End 含闭引号）。
        [[nodiscard]]
        static Token make_interp_start(StringView value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::InterpStart, lexeme, loc, value};
        }

        [[nodiscard]]
        static Token make_interp_middle(StringView value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::InterpMiddle, lexeme, loc, value};
        }

        [[nodiscard]]
        static Token make_interp_end(StringView value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::InterpEnd, lexeme, loc, value};
        }

        [[nodiscard]]
        TokenType type() const noexcept {
            return type_;
        }

        [[nodiscard]]
        StringView lexeme() const noexcept {
            if (loc_.source() == nullptr) {
                return {};
            }
            return {loc_.source()->content().data() + loc_.offset(), lexeme_len_};
        }

        [[nodiscard]]
        const SourceLoc& loc() const noexcept {
            return loc_;
        }

        [[nodiscard]]
        const TokenValue& value() const noexcept {
            return value_;
        }

        [[nodiscard]]
        bool is(const TokenType t) const noexcept {
            return type_ == t;
        }

        [[nodiscard]]
        bool is_eof() const noexcept {
            return is(TokenType::Eof);
        }

        // 仅 Integer 有效，其余返回 0。
        [[nodiscard]]
        i64 int_value() const noexcept;

        [[nodiscard]]
        f64 float_value() const noexcept;

        // 仅 String 有效，其余返回空串。
        [[nodiscard]]
        StringView string_value() const noexcept;

        // 仅 InterpStart/InterpMiddle/InterpEnd 有效（段值 = 已解析转义后的字面内容），其余返回空串。
        [[nodiscard]]
        StringView interp_value() const noexcept;

        // 调试用：形如 `Integer '42'` 的可读表示。
        [[nodiscard]]
        String to_string() const;

    private:
        Token(const TokenType type, const StringView lexeme, const SourceLoc loc, TokenValue value) noexcept :
            type_{type}, lexeme_len_{static_cast<u32>(lexeme.size())}, loc_{loc}, value_{value} {}

        TokenType  type_;
        u32        lexeme_len_;
        SourceLoc  loc_;
        TokenValue value_;
    };

    static_assert(std::is_trivially_copyable_v<Token>, "Token must be trivially copyable");

    // tokenize 的产物：token 流与字符串字面量的解析后存储。strings 里的视图在消费期内恒有效（稳定性
    // 契约见 StringArena）。
    struct TokenStream {
        List<Token> tokens;
        StringArena strings;
    };

} // namespace aria

#endif // ARIA_TOKEN_HPP
