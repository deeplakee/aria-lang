#ifndef ARIA_TOKENTYPE_HPP
#define ARIA_TOKENTYPE_HPP

#include <algorithm>
#include <iterator>
#include <limits>
#include <ranges>

#include "common.hpp"

namespace aria {

    // 词法终结符注册表：按类拆五段子表（special / literal / keyword / operator / punct），复合表
    // ARIA_TOKEN_LIST 依序拼接、段序即枚举编号序；名字表由复合表展开、以枚举值为下标，关键字
    // 纯表只由关键字子表展开。表内只能用块注释 /* */ -- 多行宏体内 // 会因续行吞掉下一行。
#define ARIA_TOKEN_SPECIAL_LIST(X) X(Eof, "")

#define ARIA_TOKEN_LITERAL_LIST(X)                                                       \
    X(Integer, "")                                                                       \
    X(Float, "")                                                                         \
    X(String, "")       /* string literal ("..." / '...'), escapes already resolved */   \
    X(InterpStart, "")  /* interpolated string: literal segment before the first hole */ \
    X(InterpMiddle, "") /* interpolated string: literal segment between two holes */     \
    X(InterpEnd, "")    /* interpolated string: literal segment after the last hole */   \
    X(Identifier, "")                                                                    \
    X(Underscore, "_") /* placeholder / wildcard */

#define ARIA_TOKEN_KEYWORD_LIST(X)                                                     \
    X(Fun, "fun")     /* function declaration / lambda */                              \
    X(Def, "def")     /* type/class declaration (static members + instance methods) */ \
    X(Var, "var")     /* variable declaration */                                       \
    X(If, "if")       /* if statement / if expression */                               \
    X(Else, "else")   /* else branch of if */                                          \
    X(While, "while") /* while loop */                                                 \
    X(For, "for")     /* for / for-in loop */                                          \
    X(In, "in")       /* for-in iteration */                                           \
    X(Break, "break")                                                                  \
    X(Continue, "continue")                                                            \
    X(Return, "return")                                                                \
    X(Import, "import") /* module import */                                            \
    X(As, "as")         /* import alias */                                             \
    X(Try, "try")       /* exception handling */                                       \
    X(Catch, "catch")                                                                  \
    X(Throw, "throw") /* throw an exception */                                         \
    X(Nil, "nil")     /* nil literal */                                                \
    X(True, "true")   /* boolean true literal */                                       \
    X(False, "false") /* boolean false literal */                                      \
    X(This, "this")   /* current instance */                                           \
    X(Super, "super") /* base class method */                                          \
    X(Match, "match") /* match statement / match expression */

#define ARIA_TOKEN_OPERATOR_LIST(X) \
    X(Plus, "+")                    \
    X(Minus, "-")                   \
    X(Star, "*")                    \
    X(Slash, "/")                   \
    X(Percent, "%")                 \
    X(PlusEqual, "+=")              \
    X(MinusEqual, "-=")             \
    X(StarEqual, "*=")              \
    X(SlashEqual, "/=")             \
    X(PercentEqual, "%=")           \
    X(Equal, "=")                   \
    X(EqualEqual, "==")             \
    X(EqualEqualEqual, "===")       \
    X(BangEqual, "!=")              \
    X(BangEqualEqual, "!==")        \
    X(Bang, "!")                    \
    X(Greater, ">")                 \
    X(GreaterEqual, ">=")           \
    X(Less, "<")                    \
    X(LessEqual, "<=")              \
    X(AndAnd, "&&")                 \
    X(OrOr, "||")                   \
    X(PlusPlus, "++")               \
    X(MinusMinus, "--")             \
    X(FatArrow, "=>")               \
    X(DotDot, "..") /* range (upper bound inclusive, a..b) */

#define ARIA_TOKEN_PUNCT_LIST(X) \
    X(LeftParen, "(")            \
    X(RightParen, ")")           \
    X(LeftBrace, "{")            \
    X(RightBrace, "}")           \
    X(LeftBracket, "[")          \
    X(RightBracket, "]")         \
    X(Comma, ",")                \
    X(Colon, ":")                \
    X(Semicolon, ";")            \
    X(Dot, ".")                  \
    X(DotDotDot, "...")

#define ARIA_TOKEN_LIST(X)      \
    ARIA_TOKEN_SPECIAL_LIST(X)  \
    ARIA_TOKEN_LITERAL_LIST(X)  \
    ARIA_TOKEN_KEYWORD_LIST(X)  \
    ARIA_TOKEN_OPERATOR_LIST(X) \
    ARIA_TOKEN_PUNCT_LIST(X)

#define ARIA_TOKEN_ENUM(name, lexeme) name,
    enum class TokenType : u8 { ARIA_TOKEN_LIST(ARIA_TOKEN_ENUM) };
#undef ARIA_TOKEN_ENUM

#define ARIA_TOKEN_NAME(name, lexeme) #name,
    // 枚举名 -> 可读名表（如 "Integer"）。
    inline constexpr StringView kTokenNames[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_NAME)};
#undef ARIA_TOKEN_NAME

    // 关键字纯表：只由关键字子表展开，子表成员资格即关键字判据。
#define ARIA_TOKEN_KEYWORD_ROW(name, lexeme) {lexeme, TokenType::name},
    inline constexpr Pair<StringView, TokenType> kKeywords[] = {ARIA_TOKEN_KEYWORD_LIST(ARIA_TOKEN_KEYWORD_ROW)};
#undef ARIA_TOKEN_KEYWORD_ROW

#undef ARIA_TOKEN_LIST
#undef ARIA_TOKEN_SPECIAL_LIST
#undef ARIA_TOKEN_LITERAL_LIST
#undef ARIA_TOKEN_KEYWORD_LIST
#undef ARIA_TOKEN_OPERATOR_LIST
#undef ARIA_TOKEN_PUNCT_LIST

    [[nodiscard]]
    constexpr StringView to_string(const TokenType type) noexcept {
        const auto index = std::to_underlying(type);
        ASSERT(index < std::size(kTokenNames), "TokenType out of range");
        return kTokenNames[index];
    }

    // 关键字拼写长度区间（自纯表量出）：区间外 lexeme 不可能命中，查找免遍历早退。
    inline constexpr Pair<usize, usize> kKeywordLenRange = [] {
        usize min_len = std::numeric_limits<usize>::max();
        usize max_len = std::numeric_limits<usize>::min();
        for (const auto& key: kKeywords | std::views::keys) {
            const usize len = key.size();
            min_len         = std::min(min_len, len);
            max_len         = std::max(max_len, len);
        }
        return Pair{min_len, max_len};
    }();

    // 关键字查表：lexeme 为关键字返回其 TokenType，否则 nullopt；调用前 lexer 已切出完整 identifier。
    [[nodiscard]]
    constexpr Opt<TokenType> lookup_keyword(const StringView lexeme) noexcept {
        if (lexeme.size() < kKeywordLenRange.first || lexeme.size() > kKeywordLenRange.second) {
            return std::nullopt;
        }
        for (const auto& [spelling, type]: kKeywords) {
            if (spelling == lexeme) {
                return type;
            }
        }
        return std::nullopt;
    }

} // namespace aria

#endif // ARIA_TOKENTYPE_HPP
