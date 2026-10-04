#ifndef ARIA_TOKENTYPE_HPP
#define ARIA_TOKENTYPE_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 词法终结符注册表：每行 X(枚举名, 拼写, 是否关键字)，枚举隐式连续编号（下标即
    // std::to_underlying(type)），名字表与拼写表由本表同源展开。表内只能用块注释
    // /* */ -- 多行宏体内 // 会因续行吞掉下一行。
#define ARIA_TOKEN_LIST(X)                                                                      \
    /* --- special --- */                                                                       \
    X(Eof, "", false)                                                                           \
    /* --- literals --- */                                                                      \
    X(Integer, "", false)                                                                       \
    X(Float, "", false)                                                                         \
    X(String, "", false)       /* string literal ("..." / '...'), escapes already resolved */   \
    X(InterpStart, "", false)  /* interpolated string: literal segment before the first hole */ \
    X(InterpMiddle, "", false) /* interpolated string: literal segment between two holes */     \
    X(InterpEnd, "", false)    /* interpolated string: literal segment after the last hole */   \
    X(Identifier, "", false)                                                                    \
    X(Underscore, "_", false) /* placeholder / wildcard */                                      \
    /* --- keywords --- */                                                                      \
    X(Fun, "fun", true)     /* function declaration / lambda */                                 \
    X(Def, "def", true)     /* type/class declaration (static members + instance methods) */    \
    X(Var, "var", true)     /* variable declaration */                                          \
    X(If, "if", true)       /* if statement / if expression */                                  \
    X(Else, "else", true)   /* else branch of if */                                             \
    X(While, "while", true) /* while loop */                                                    \
    X(For, "for", true)     /* for / for-in loop */                                             \
    X(In, "in", true)       /* for-in iteration */                                              \
    X(Break, "break", true)                                                                     \
    X(Continue, "continue", true)                                                               \
    X(Return, "return", true)                                                                   \
    X(Import, "import", true) /* module import */                                               \
    X(As, "as", true)         /* import alias */                                                \
    X(Try, "try", true)       /* exception handling */                                          \
    X(Catch, "catch", true)                                                                     \
    X(Throw, "throw", true) /* throw an exception */                                            \
    X(Nil, "nil", true)     /* nil literal */                                                   \
    X(True, "true", true)   /* boolean true literal */                                          \
    X(False, "false", true) /* boolean false literal */                                         \
    X(This, "this", true)   /* current instance */                                              \
    X(Super, "super", true) /* base class method */                                             \
    X(Match, "match", true) /* match statement / match expression */                            \
    /* --- operators --- */                                                                     \
    X(Plus, "+", false)                                                                         \
    X(Minus, "-", false)                                                                        \
    X(Star, "*", false)                                                                         \
    X(Slash, "/", false)                                                                        \
    X(Percent, "%", false)                                                                      \
    X(PlusEqual, "+=", false)                                                                   \
    X(MinusEqual, "-=", false)                                                                  \
    X(StarEqual, "*=", false)                                                                   \
    X(SlashEqual, "/=", false)                                                                  \
    X(PercentEqual, "%=", false)                                                                \
    X(Equal, "=", false)                                                                        \
    X(EqualEqual, "==", false)                                                                  \
    X(EqualEqualEqual, "===", false)                                                            \
    X(BangEqual, "!=", false)                                                                   \
    X(BangEqualEqual, "!==", false)                                                             \
    X(Bang, "!", false)                                                                         \
    X(Greater, ">", false)                                                                      \
    X(GreaterEqual, ">=", false)                                                                \
    X(Less, "<", false)                                                                         \
    X(LessEqual, "<=", false)                                                                   \
    X(AndAnd, "&&", false)                                                                      \
    X(OrOr, "||", false)                                                                        \
    X(PlusPlus, "++", false)                                                                    \
    X(MinusMinus, "--", false)                                                                  \
    X(FatArrow, "=>", false)                                                                    \
    X(DotDot, "..", false) /* range (upper bound inclusive, a..b) */                            \
    /* --- punctuation --- */                                                                   \
    X(LeftParen, "(", false)                                                                    \
    X(RightParen, ")", false)                                                                   \
    X(LeftBrace, "{", false)                                                                    \
    X(RightBrace, "}", false)                                                                   \
    X(LeftBracket, "[", false)                                                                  \
    X(RightBracket, "]", false)                                                                 \
    X(Comma, ",", false)                                                                        \
    X(Colon, ":", false)                                                                        \
    X(Semicolon, ";", false)                                                                    \
    X(Dot, ".", false)                                                                          \
    X(DotDotDot, "...", false)

#define ARIA_TOKEN_ENUM(name, lexeme, is_keyword) name,
    enum class TokenType : u8 { ARIA_TOKEN_LIST(ARIA_TOKEN_ENUM) };
#undef ARIA_TOKEN_ENUM

#define ARIA_TOKEN_NAME(name, lexeme, is_keyword) #name,
    // 枚举名 -> 可读名表（如 "Integer"）。
    inline constexpr StringView kTokenNames[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_NAME)};
#undef ARIA_TOKEN_NAME

#define ARIA_TOKEN_LEXEME(name, lexeme, is_keyword) {lexeme, is_keyword},
    // 拼写表：下标即 std::to_underlying(type)，表项 (固定拼写, 是否关键字)。
    inline constexpr Pair<StringView, bool> kTokenLexemes[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_LEXEME)};
#undef ARIA_TOKEN_LEXEME

#undef ARIA_TOKEN_LIST

    [[nodiscard]]
    constexpr StringView to_string(const TokenType type) noexcept {
        const auto index = std::to_underlying(type);
        ASSERT(index < std::size(kTokenNames), "TokenType out of range");
        return kTokenNames[index];
    }

    // 关键字查表：lexeme 为关键字返回其 TokenType，否则 nullopt；调用前 lexer 已切出完整 identifier。
    [[nodiscard]]
    constexpr Opt<TokenType> lookup_keyword(const StringView lexeme) noexcept {
        for (usize index = 0; index < std::size(kTokenLexemes); ++index) {
            const auto& [spelling, is_keyword] = kTokenLexemes[index];
            if (is_keyword && spelling == lexeme) {
                return static_cast<TokenType>(index);
            }
        }
        return std::nullopt;
    }

} // namespace aria

#endif // ARIA_TOKENTYPE_HPP
