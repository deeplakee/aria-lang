#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <variant>
#include "common.hpp"
#include "compile/TokenType.hpp"
#include "util/source_file.hpp"

namespace aria {
    using src::SourceLoc;

    // 字面量 token 携带的解析值；Integer 以 i64 容纳（i48 值域，越界在语义阶段拒绝），String 已解析转义。
    using TokenValue = std::variant<std::monostate, i64, f64, String>;

    // 词法单元：lexer 产出的最小语法单位。lexeme 借用源码文本，不得越过所引 SourceFile 的存活期。
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

        // value 为已解析转义后的字符串内容；lexeme 保留原始源码文本。
        [[nodiscard]]
        static Token make_string(String value, const StringView lexeme, const SourceLoc loc) {
            return Token{TokenType::String, lexeme, loc, std::move(value)};
        }

        [[nodiscard]]
        TokenType type() const noexcept {
            return type_;
        }

        [[nodiscard]]
        StringView lexeme() const noexcept {
            return lexeme_;
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

        // 调试用：形如 `Integer '42'` 的可读表示。
        [[nodiscard]]
        String to_string() const;

    private:
        Token(const TokenType type, const StringView lexeme, const SourceLoc loc, TokenValue value) noexcept :
            type_{type}, loc_{loc}, lexeme_{lexeme}, value_{std::move(value)} {}

        TokenType  type_;
        SourceLoc  loc_;
        StringView lexeme_;
        TokenValue value_;
    };

} // namespace aria

#endif // ARIA_TOKEN_HPP
