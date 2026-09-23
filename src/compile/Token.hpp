#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <variant>
#include "common.hpp"
#include "compile/TokenType.hpp"
#include "util/source_file.hpp"

namespace aria {
    // 将 SourceLoc 引入 aria 命名空间，便于本模块直接使用（source_file 相关类型
    // 位于 aria::src 下，引用需分别 using）。
    using src::SourceLoc;

    // 字面量 token 携带的解析值：
    //   - monostate：非字面量 token（关键字 / 运算符 / 标点 / EOF）
    //   - i64：Integer（文法约定 int 为 i48，此处用 i64 容纳，越界在语义阶段处理）
    //   - f64：Float
    //   - String：String 字面量（转义已解析）
    using TokenValue = std::variant<std::monostate, i64, f64, String>;

    // 词法单元：lexer 产出的最小语法单位。
    // lexeme 为指向源码内容的 StringView，不得比所引用的 SourceFile 活得更久
    // （见 source_file.hpp 的生命周期约束）。字面量值经 lexer 解析后存于 value_，
    // 后续阶段无需再次扫描 lexeme。
    class Token {
    public:
        // 默认构造：EOF、空 loc / lexeme、无字面量值。供容器（如 List<Token>）预留槽位。
        Token() noexcept : Token{TokenType::Eof, {}, {}, {}} {}

        // 构造一个不带字面量值的 token（关键字 / 运算符 / 标点 / Identifier /
        // Underscore / Eof）。
        Token(const TokenType type, const StringView lexeme, const SourceLoc loc) noexcept :
            Token{type, lexeme, loc, {}} {}

        // 字面量 token 的工厂
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

        // 取整数/浮点字面量值（仅对应类型有效，其余返回 0）。
        [[nodiscard]]
        i64 int_value() const noexcept;

        [[nodiscard]]
        f64 float_value() const noexcept;

        // 取已解析字符串内容（仅 String token 有效，其余返回空串）。
        [[nodiscard]]
        StringView string_value() const noexcept;

        // 调试用：返回形如 `Integer '42'` 的可读表示。
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
