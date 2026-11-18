#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <type_traits>

#include "common.hpp"
#include "compile/TokenType.hpp"

namespace aria {

    // 词法期记录展开长度，消费端据此定容并解码。
    struct StringShape {
        u32  decoded_len = 0; // 展开后内容长度（词法期记账）
        bool is_escape   = false;
    };

    // 活跃成员由 TokenType 判别；整数域检查延至语义阶段，字符串仅记录 StringShape。
    union TokenValue {
        i64         int_;
        f64         float_;
        StringShape shape_;
    };

    // lexeme 借用源缓冲，EOF 指向尾部哨兵；Parser 按其偏移派生位置。
    // 源缓冲须在 token 及其借出视图使用期间保持存活且地址稳定。
    class Token {
    public:
        Token() noexcept : Token{TokenType::Eof, StringView{}, {}} {}

        Token(const TokenType type, const StringView lexeme) noexcept : Token{type, lexeme, {}} {}

        [[nodiscard]]
        static Token make_integer(const i64 value, const StringView lexeme) noexcept {
            return Token{TokenType::Integer, lexeme, TokenValue{.int_ = value}};
        }

        [[nodiscard]]
        static Token make_float(const f64 value, const StringView lexeme) noexcept {
            return Token{TokenType::Float, lexeme, TokenValue{.float_ = value}};
        }

        // lexeme 包含两端引号。
        [[nodiscard]]
        static Token make_string(const StringView lexeme, const StringShape shape) noexcept {
            return Token{TokenType::String, lexeme, TokenValue{.shape_ = shape}};
        }

        // Start/Middle 不含边界，End 含闭引号。
        [[nodiscard]]
        static Token make_interp_start(const StringView lexeme, const StringShape shape) noexcept {
            return Token{TokenType::InterpStart, lexeme, TokenValue{.shape_ = shape}};
        }

        [[nodiscard]]
        static Token make_interp_middle(const StringView lexeme, const StringShape shape) noexcept {
            return Token{TokenType::InterpMiddle, lexeme, TokenValue{.shape_ = shape}};
        }

        [[nodiscard]]
        static Token make_interp_end(const StringView lexeme, const StringShape shape) noexcept {
            return Token{TokenType::InterpEnd, lexeme, TokenValue{.shape_ = shape}};
        }

        [[nodiscard]]
        TokenType type() const noexcept {
            return type_;
        }

        [[nodiscard]]
        StringView lexeme() const noexcept {
            return {data_, len_};
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

        // 字符串族返回未展开的内层原文视图，其余返回空视图。
        [[nodiscard]]
        StringView string_value() const noexcept;

        [[nodiscard]]
        StringShape shape() const noexcept;

        [[nodiscard]]
        String to_string() const;

    private:
        Token(const TokenType type, const StringView lexeme, TokenValue value) noexcept :
            type_{type}, len_{static_cast<u32>(lexeme.size())}, data_{lexeme.data()}, value_{value} {}

        TokenType   type_;
        u32         len_;
        const char* data_;
        TokenValue  value_;
    };

    static_assert(std::is_trivially_copyable_v<Token>, "Token must be trivially copyable");

} // namespace aria

#endif // ARIA_TOKEN_HPP
