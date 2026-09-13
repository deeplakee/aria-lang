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
//   - 特殊（EOF）
//
// 类型全量注册表（单一事实源，同 code.hpp 的 ARIA_OPCODE_LIST / ErrorCode.hpp 的
// ARIA_ERROR_LIST 风格）：TokenType 枚举与可读名表 kTokenNames 都由 ARIA_TOKEN_LIST(X)
// 展开，新增类型加一行 X(名字) 即收口，名字串经 # 派生，无第二处手写。隐式连续编号，
// 下标即 std::to_underlying(type)。逐值注释用块注释（行注释会吞续行符）。
#define ARIA_TOKEN_LIST(X)                                       \
    /* --- 特殊 --- */                                           \
    X(Eof)                                                       \
    /* --- 字面量 --- */                                         \
    X(Integer)                                                   \
    X(Float)                                                     \
    X(String) /* 字符串字面量（"..." / '...'），转义已解析 */    \
    X(Identifier)                                                \
    X(Underscore) /* 单独的 "_"，占位/通配符 */                  \
    /* --- 关键字 --- */                                         \
    X(Fun)      /* "fun"   函数声明 / lambda 表达式 */           \
    X(Def)      /* "def"   类型/类声明（静态成员 + 实例方法） */ \
    X(Var)      /* "var"   变量声明 */                           \
    X(If)       /* "if"    if 语句 / if 表达式 */                \
    X(Else)     /* "else"  if 的 else 分支 */                    \
    X(While)    /* "while" while 循环 */                         \
    X(For)      /* "for"   for / for-in 循环 */                  \
    X(In)       /* "in"    for-in 遍历 */                        \
    X(Break)    /* "break" */                                    \
    X(Continue) /* "continue" */                                 \
    X(Return)   /* "return" */                                   \
    X(Import)   /* "import" 模块导入 */                          \
    X(As)       /* "as"    import 的别名 */                      \
    X(Try)      /* "try"   异常处理 */                           \
    X(Catch)    /* "catch" */                                    \
    X(Throw)    /* "throw"  抛出异常 */                          \
    X(Print)    /* "print"  打印语句 */                          \
    X(Nil)      /* "nil"    空值字面量 */                        \
    X(True)     /* "true"   布尔真字面量 */                      \
    X(False)    /* "false"  布尔假字面量 */                      \
    X(This)     /* "this"   当前实例 */                          \
    X(Super)    /* "super"  父类方法 */                          \
    X(Match)    /* "match"  match 语句 / match 表达式 */         \
    /* --- 运算符 --- */                                         \
    X(Plus)            /* + */                                   \
    X(Minus)           /* - */                                   \
    X(Star)            /* * */                                   \
    X(Slash)           /* / */                                   \
    X(Percent)         /* % */                                   \
    X(PlusEqual)       /* += */                                  \
    X(MinusEqual)      /* -= */                                  \
    X(StarEqual)       /* *= */                                  \
    X(SlashEqual)      /* /= */                                  \
    X(PercentEqual)    /* %= */                                  \
    X(Equal)           /* = */                                   \
    X(EqualEqual)      /* == */                                  \
    X(EqualEqualEqual) /* === */                                 \
    X(BangEqual)       /* != */                                  \
    X(BangEqualEqual)  /* !== */                                 \
    X(Bang)            /* ! */                                   \
    X(Greater)         /* > */                                   \
    X(GreaterEqual)    /* >= */                                  \
    X(Less)            /* < */                                   \
    X(LessEqual)       /* <= */                                  \
    X(AndAnd)          /* && */                                  \
    X(OrOr)            /* || */                                  \
    X(PlusPlus)        /* ++ */                                  \
    X(MinusMinus)      /* -- */                                  \
    X(FatArrow)        /* => */                                  \
    X(DotDot)          /* ..  区间（含上界，a..b） */            \
    /* --- 标点 --- */                                           \
    X(LeftParen)    /* ( */                                      \
    X(RightParen)   /* ) */                                      \
    X(LeftBrace)    /* { */                                      \
    X(RightBrace)   /* } */                                      \
    X(LeftBracket)  /* [ */                                      \
    X(RightBracket) /* ] */                                      \
    X(Comma)        /* , */                                      \
    X(Colon)        /* : */                                      \
    X(Semicolon)    /* ; */                                      \
    X(Dot)          /* . */                                      \
    X(DotDotDot)    /* ... */

#define ARIA_TOKEN_ENUM(name) name,
    enum class TokenType : u8 { ARIA_TOKEN_LIST(ARIA_TOKEN_ENUM) };
#undef ARIA_TOKEN_ENUM

#define ARIA_TOKEN_NAME(name) #name,
    // 类型可读名表（如 "Integer"、"FatArrow"）；与枚举同源生成（见 ARIA_TOKEN_LIST 注）。
    inline constexpr StringView kTokenNames[] = {ARIA_TOKEN_LIST(ARIA_TOKEN_NAME)};
#undef ARIA_TOKEN_NAME

#undef ARIA_TOKEN_LIST

    // TokenType 的可读名映射（如 "Integer"、"FatArrow"）；Token::to_string 渲染与
    // Parser 错误信息复用。非法值（u8 强转越界）为编程错误，ASSERT 拦截（同 ErrorCode）。
    [[nodiscard]]
    constexpr StringView to_string(const TokenType type) noexcept {
        const auto index = std::to_underlying(type);
        ASSERT(index < std::size(kTokenNames), "TokenType out of range");
        return kTokenNames[index];
    }

    // 关键字表：lexeme -> TokenType。与文法「关键字」清单（docs/grammar.txt 末尾）一致，
    // 大小写敏感；逻辑运算符为 &&/||/!（AndAnd/OrOr/Bang），无 and/or/not 关键字别名。
    // 关键字均为 ASCII，使用普通字符串字面量（u8"..." 会得到 char8_t[]，
    // 无法构造 string_view<char>）。
    struct KeywordEntry {
        StringView lexeme;
        TokenType  type;
    };

    inline constexpr KeywordEntry kKeywords[] = {
            {"as", TokenType::As},         {"break", TokenType::Break},
            {"catch", TokenType::Catch},   {"continue", TokenType::Continue},
            {"def", TokenType::Def},       {"else", TokenType::Else},
            {"for", TokenType::For},       {"fun", TokenType::Fun},
            {"if", TokenType::If},         {"import", TokenType::Import},
            {"in", TokenType::In},         {"match", TokenType::Match},
            {"nil", TokenType::Nil},       {"print", TokenType::Print},
            {"return", TokenType::Return}, {"super", TokenType::Super},
            {"this", TokenType::This},     {"throw", TokenType::Throw},
            {"try", TokenType::Try},       {"var", TokenType::Var},
            {"while", TokenType::While},   {"true", TokenType::True},
            {"false", TokenType::False},
    };

    // 关键字查表：若 lexeme 是关键字则返回对应 TokenType，否则返回 std::nullopt。
    // 大小写敏感，最长匹配由 lexer 保证（调用前已切出完整 identifier）。顺序扫描即可（关键字数量少）。
    [[nodiscard]]
    constexpr Opt<TokenType> lookup_keyword(const StringView lexeme) noexcept {
        // 绑定名取 word 而非 lexeme，避免遮蔽外层参数 lexeme 导致自比恒真。
        for (const auto& [word, type]: kKeywords) {
            if (word == lexeme) {
                return type;
            }
        }
        return std::nullopt;
    }

} // namespace aria

#endif // ARIA_TOKENTYPE_HPP
