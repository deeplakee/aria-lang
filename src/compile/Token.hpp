#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <type_traits>

#include "common.hpp"
#include "compile/TokenType.hpp"
#include "util/source_file.hpp"

namespace aria {
    using src::SourceLoc;

    // 字符串字面量段的消费形态：转义展开后内容长度 + 是否含转义。Lexer 扫描时记账，随 token 与
    // 字面节点一路走到代码生成端（解码口据此分支与定容）。
    struct StringShape {
        u32  decoded_len = 0; // 展开后内容长度（词法期记账）
        bool is_escape   = false;
    };

    // 字面量 token 携带的解析值；活跃成员由 TokenType 判别（Integer->int_、Float->float_、String
    // 与三个 Interp 段->shape_、其余为空），故 union 不落 tag。Integer 以 i64 容纳（i48 值域，
    // 越界在语义阶段拒绝）；字符串族载荷 = StringShape（词法期记账，内容在消费端展开），打包借
    // 长度位后的衬垫槽，与 8B 槽同宽。
    union TokenValue {
        i64         int_;
        f64         float_;
        StringShape shape_;
    };

    // 词法单元：lexer 产出的最小语法单位。lexeme 不落指针，只存字节数，读时按 loc 自源缓冲重建
    // （词法期各 token 的 lexeme 区间恒以 loc 的偏移为起点）；字符串族另持转义标记与展开后长度，
    // 内层原文视图经 string_value() 剥边界取得，同样借自源缓冲。借出的视图均不得越过所引对象的
    // 存活期。
    class Token {
    public:
        Token() noexcept : Token{TokenType::Eof, {}, {}, {}} {}

        Token(const TokenType type, const StringView lexeme, const SourceLoc loc) noexcept :
            Token{type, lexeme, loc, {}} {}

        [[nodiscard]]
        static Token make_integer(const i64 value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::Integer, lexeme, loc, TokenValue{.int_ = value}};
        }

        [[nodiscard]]
        static Token make_float(const f64 value, const StringView lexeme, const SourceLoc loc) noexcept {
            return Token{TokenType::Float, lexeme, loc, TokenValue{.float_ = value}};
        }

        // 字符串字面量：lexeme 为含两端引号的原文，shape 为字面段消费形态（词法期记账）。
        [[nodiscard]]
        static Token make_string(const StringView lexeme, const SourceLoc loc, const StringShape shape) noexcept {
            return Token{TokenType::String, lexeme, loc, TokenValue{.shape_ = shape}};
        }

        // 插值串字面段三厂：lexeme 为该段原文（Start/Middle 不含边界，End 含闭引号），shape 语义
        // 同 make_string。
        [[nodiscard]]
        static Token make_interp_start(const StringView lexeme, const SourceLoc loc, const StringShape shape) noexcept {
            return Token{TokenType::InterpStart, lexeme, loc, TokenValue{.shape_ = shape}};
        }

        [[nodiscard]]
        static Token make_interp_middle(const StringView lexeme, const SourceLoc loc,
                                        const StringShape shape) noexcept {
            return Token{TokenType::InterpMiddle, lexeme, loc, TokenValue{.shape_ = shape}};
        }

        [[nodiscard]]
        static Token make_interp_end(const StringView lexeme, const SourceLoc loc, const StringShape shape) noexcept {
            return Token{TokenType::InterpEnd, lexeme, loc, TokenValue{.shape_ = shape}};
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

        // 仅 String 与三个 Interp 段有效：字面内层原文视图（剥去引号/档边界，转义未展开），借自
        // 源缓冲；其余返回空视图。
        [[nodiscard]]
        StringView string_value() const noexcept;

        // 仅字符串族有效：字面段消费形态，其余返回零值。
        [[nodiscard]]
        StringShape shape() const noexcept;

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

    // 平凡可拷贝是 token 表零搬移与读侧单缓存行的前提。
    static_assert(std::is_trivially_copyable_v<Token>, "Token must be trivially copyable");

} // namespace aria

#endif // ARIA_TOKEN_HPP
