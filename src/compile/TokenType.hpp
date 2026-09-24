#ifndef ARIA_TOKENTYPE_HPP
#define ARIA_TOKENTYPE_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 词法单元类型。覆盖文法（docs/grammar.txt）中的全部终结符：
    //   - 关键字（fun/def/var/... 共 23 个，见文末「关键字」清单）
    //   - 运算符（算术 / 复合赋值 / 比较 / 逻辑 / 自增自减 / =>）
    //   - 标点（括号 / 逗号 / 冒号 / 分号 / 点 / ...）
    //   - 字面量（整数 / 浮点 / 字符串 / 标识符 / _ 占位符）
    //   - 特殊（EOF）类型全量注册表（单一事实源，同 code.hpp 的 ARIA_OPCODE_LIST / ErrorCode.hpp 的 ARIA_ERROR_LIST 风
    //     格）：TokenType 枚举、可读名表 kTokenNames 与拼写表 kTokenLexemes 都由 ARIA_TOKEN_LIST(X) 展开，新增类型加
    //     一行 X(名字, 拼写, 是否关键字) 即收口，名字串经 # 派生，无第二处手写。隐式连续编号，下标即 std::
    //     to_underlying(type)。第二列拼写：关键字/运算符/标点/下划线为字面拼写，字面量与 EOF 无固定拼写记空串；第三列
    //     标记该类型是否参与关键字查表（仅关键字 true）。逐值注释用块注释（行注释会吞续行符）。
#define ARIA_TOKEN_LIST(X)                                                                   \
    /* --- special --- */                                                                    \
    X(Eof, "", false)                                                                        \
    /* --- literals --- */                                                                   \
    X(Integer, "", false)                                                                    \
    X(Float, "", false)                                                                      \
    X(String, "", false) /* string literal ("..." / '...'), escapes already resolved */      \
    X(Identifier, "", false)                                                                 \
    X(Underscore, "_", false) /* placeholder / wildcard */                                   \
    /* --- keywords --- */                                                                   \
    X(Fun, "fun", true)     /* function declaration / lambda */                              \
    X(Def, "def", true)     /* type/class declaration (static members + instance methods) */ \
    X(Var, "var", true)     /* variable declaration */                                       \
    X(If, "if", true)       /* if statement / if expression */                               \
    X(Else, "else", true)   /* else branch of if */                                          \
    X(While, "while", true) /* while loop */                                                 \
    X(For, "for", true)     /* for / for-in loop */                                          \
    X(In, "in", true)       /* for-in iteration */                                           \
    X(Break, "break", true)                                                                  \
    X(Continue, "continue", true)                                                            \
    X(Return, "return", true)                                                                \
    X(Import, "import", true) /* module import */                                            \
    X(As, "as", true)         /* import alias */                                             \
    X(Try, "try", true)       /* exception handling */                                       \
    X(Catch, "catch", true)                                                                  \
    X(Throw, "throw", true) /* throw an exception */                                         \
    X(Nil, "nil", true)     /* nil literal */                                                \
    X(True, "true", true)   /* boolean true literal */                                       \
    X(False, "false", true) /* boolean false literal */                                      \
    X(This, "this", true)   /* current instance */                                           \
    X(Super, "super", true) /* base class method */                                          \
    X(Match, "match", true) /* match statement / match expression */                         \
    /* --- operators --- */                                                                  \
    X(Plus, "+", false)                                                                      \
    X(Minus, "-", false)                                                                     \
    X(Star, "*", false)                                                                      \
    X(Slash, "/", false)                                                                     \
    X(Percent, "%", false)                                                                   \
    X(PlusEqual, "+=", false)                                                                \
    X(MinusEqual, "-=", false)                                                               \
    X(StarEqual, "*=", false)                                                                \
    X(SlashEqual, "/=", false)                                                               \
    X(PercentEqual, "%=", false)                                                             \
    X(Equal, "=", false)                                                                     \
    X(EqualEqual, "==", false)                                                               \
    X(EqualEqualEqual, "===", false)                                                         \
    X(BangEqual, "!=", false)                                                                \
    X(BangEqualEqual, "!==", false)                                                          \
    X(Bang, "!", false)                                                                      \
    X(Greater, ">", false)                                                                   \
    X(GreaterEqual, ">=", false)                                                             \
    X(Less, "<", false)                                                                      \
    X(LessEqual, "<=", false)                                                                \
    X(AndAnd, "&&", false)                                                                   \
    X(OrOr, "||", false)                                                                     \
    X(PlusPlus, "++", false)                                                                 \
    X(MinusMinus, "--", false)                                                               \
    X(FatArrow, "=>", false)                                                                 \
    X(DotDot, "..", false) /* range (upper bound inclusive, a..b) */                         \
    /* --- punctuation --- */                                                                \
    X(LeftParen, "(", false)                                                                 \
    X(RightParen, ")", false)                                                                \
    X(LeftBrace, "{", false)                                                                 \
    X(RightBrace, "}", false)                                                                \
    X(LeftBracket, "[", false)                                                               \
    X(RightBracket, "]", false)                                                              \
    X(Comma, ",", false)                                                                     \
    X(Colon, ":", false)                                                                     \
    X(Semicolon, ";", false)                                                                 \
    X(Dot, ".", false)                                                                       \
    X(DotDotDot, "...", false)

#define ARIA_TOKEN_ENUM(name, lexeme, is_keyword) name,
    enum class TokenType : u8 { ARIA_TOKEN_LIST(ARIA_TOKEN_ENUM) };
#undef ARIA_TOKEN_ENUM

#define ARIA_TOKEN_NAME(name, lexeme, is_keyword) #name,
    // 类型可读名表（如 "Integer"、"FatArrow"）；与枚举同源生成（见 ARIA_TOKEN_LIST 注）。
    inline constexpr StringView kTokenNames[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_NAME)};
#undef ARIA_TOKEN_NAME

#define ARIA_TOKEN_LEXEME(name, lexeme, is_keyword) {lexeme, is_keyword},
    // 拼写表（下标即 std::to_underlying(type)；与枚举同源生成，见 ARIA_TOKEN_LIST 注）。
    // 表项 (固定拼写, 是否关键字)，关键字行供 lookup_keyword 消费。
    inline constexpr Pair<StringView, bool> kTokenLexemes[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_LEXEME)};
#undef ARIA_TOKEN_LEXEME

#undef ARIA_TOKEN_LIST

    // TokenType 的可读名映射（如 "Integer"、"FatArrow"）；Token::to_string 渲染（调试面）用。
    // 报错文案不用它 -- 违规片段取 Token::lexeme() 的源码原片段。非法值（u8 强转越界）为编程
    // 错误，ASSERT 拦截（同 ErrorCode）。
    [[nodiscard]]
    constexpr StringView to_string(const TokenType type) noexcept {
        const auto index = std::to_underlying(type);
        ASSERT(index < std::size(kTokenNames), "TokenType out of range");
        return kTokenNames[index];
    }

    // 关键字查表：lexeme 为关键字则返回对应 TokenType，否则返回 std::nullopt。顺序扫
    // kTokenLexemes 的关键字行（is_keyword 置位）。大小写敏感，最长匹配由 lexer 保证
    // （调用前已切出完整 identifier）。
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
