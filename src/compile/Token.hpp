#ifndef ARIA_TOKEN_HPP
#define ARIA_TOKEN_HPP

#include <variant>
#include "common.hpp"
#include "util/source_file.hpp"

namespace aria {
    // 将 SourceLoc 引入 aria 命名空间，便于本模块直接使用（source_file 相关类型
    // 位于 aria::src 下，引用需分别 using）。
    using src::SourceLoc;

    // 词法单元类型。覆盖文法（docs/grammar.txt）中的全部终结符：
    //   - 关键字（fun/def/var/... 共 23 个，见文末「关键字」清单）
    //   - 运算符（算术 / 复合赋值 / 比较 / 逻辑 / 自增自减 / =>）
    //   - 标点（括号 / 逗号 / 冒号 / 分号 / 点 / ...）
    //   - 字面量（整数 / 浮点 / 字符串 / 标识符 / _ 占位符）
    //   - 特殊（EOF / 词法错误）
    enum class TokenType : u8 {
        // --- 特殊 ---
        Eof,
        Error,

        // --- 字面量 ---
        Integer,
        Float,
        String, // 字符串字面量（"..." / '...'），转义已解析
        Identifier,
        Underscore, // 单独的 "_"，占位/通配符

        // --- 关键字 ---
        Fun,      // "fun"   函数声明 / lambda 表达式
        Def,      // "def"   类型/类声明（静态成员 + 实例方法）
        Var,      // "var"   变量声明
        If,       // "if"    if 语句 / if 表达式
        Else,     // "else"  if 的 else 分支
        While,    // "while" while 循环
        For,      // "for"   for / for-in 循环
        In,       // "in"    for-in 遍历
        Break,    // "break"
        Continue, // "continue"
        Return,   // "return"
        Import,   // "import" 模块导入
        As,       // "as"     import 的别名
        Try,      // "try"    异常处理
        Catch,    // "catch"
        Throw,    // "throw"  抛出异常
        Print,    // "print"  打印语句
        Nil,      // "nil"    空值字面量
        True,     // "true"   布尔真字面量
        False,    // "false"  布尔假字面量
        This,     // "this"   当前实例
        Super,    // "super"  父类方法
        Match,    // "match"  match 语句 / match 表达式

        // --- 运算符 ---
        Plus,            // +
        Minus,           // -
        Star,            // *
        Slash,           // /
        Percent,         // %
        PlusEqual,       // +=
        MinusEqual,      // -=
        StarEqual,       // *=
        SlashEqual,      // /=
        PercentEqual,    // %=
        Equal,           // =
        EqualEqual,      // ==
        EqualEqualEqual, // ===
        BangEqual,       // !=
        BangEqualEqual,  // !==
        Bang,            // !
        Greater,         // >
        GreaterEqual,    // >=
        Less,            // <
        LessEqual,       // <=
        AndAnd,          // &&
        OrOr,            // ||
        PlusPlus,        // ++
        MinusMinus,      // --
        FatArrow,        // =>
        DotDot,          // ..  区间（含上界，a..b）

        // --- 标点 ---
        LeftParen,    // (
        RightParen,   // )
        LeftBrace,    // {
        RightBrace,   // }
        LeftBracket,  // [
        RightBracket, // ]
        Comma,        // ,
        Colon,        // :
        Semicolon,    // ;
        Dot,          // .
        DotDotDot,    // ...
    };

    // 字面量 token 携带的解析值：
    //   - monostate：非字面量 token（关键字 / 运算符 / 标点 / EOF）
    //   - i64：Integer（文法约定 int 为 i48，此处用 i64 容纳，越界在语义阶段处理）
    //   - f64：Float
    //   - String：String 字面量（转义已解析）；Error token 复用此槽存放错误信息
    using TokenValue = std::variant<std::monostate, i64, f64, String>;

    // 词法单元：lexer 产出的最小语法单位。
    //
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

        // --- 字面量 token 的工厂 ---
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

        // 词法错误 token：message 存于 value_ 的 String 槽，lexeme 留空。
        [[nodiscard]]
        static Token make_error(String message, const SourceLoc loc) {
            return Token{TokenType::Error, {}, loc, std::move(message)};
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
            return type_ == TokenType::Eof;
        }

        [[nodiscard]]
        bool is_error() const noexcept {
            return type_ == TokenType::Error;
        }

        // 取错误信息（仅 Error token 有效）。
        [[nodiscard]]
        StringView error_message() const noexcept;

        // 取整数/浮点字面量值（仅对应类型有效，其余返回 0）。
        [[nodiscard]]
        i64 int_value() const noexcept;

        [[nodiscard]]
        f64 float_value() const noexcept;

        // 取已解析字符串内容（仅 String token 有效，其余返回空串）。
        [[nodiscard]]
        StringView string_value() const noexcept;

        // --- 类型分类（基于 TokenType） ---
        // 词法层「值/名」分类（Integer/Float/String/Identifier/Underscore），与
        // is_keyword/is_operator/is_punctuation 正交互补。注意这是词法概念，与文法产生式
        // literal（编译期常量，更窄）不同--文法 literal 的判断留给 parser。
        [[nodiscard]]
        bool is_literal() const noexcept;

        [[nodiscard]]
        bool is_keyword() const noexcept;

        [[nodiscard]]
        bool is_operator() const noexcept;

        [[nodiscard]]
        bool is_punctuation() const noexcept;

        // 调试用：返回形如 `Integer '42'` 的可读表示。
        [[nodiscard]]
        String to_string() const;

    private:
        // 全参私有构造函数：所有初始化在此收口。
        Token(const TokenType type, const StringView lexeme, const SourceLoc loc, TokenValue value) noexcept :
            type_{type}, loc_{loc}, lexeme_{lexeme}, value_{std::move(value)} {}

        TokenType  type_;
        SourceLoc  loc_;
        StringView lexeme_;
        TokenValue value_;
    };

    // 将 TokenType 转为可读名称（如 "Integer"、"FatArrow"、"LeftParen"），用于错误信息与调试。
    [[nodiscard]]
    StringView token_type_name(TokenType type) noexcept;

    // 关键字查表：若 lexeme 是关键字则返回对应 TokenType，否则返回 std::nullopt。
    // 大小写敏感，最长匹配由 lexer 保证（调用前已切出完整 identifier）。
    [[nodiscard]]
    Opt<TokenType> lookup_keyword(StringView lexeme) noexcept;

} // namespace aria

#endif // ARIA_TOKEN_HPP
