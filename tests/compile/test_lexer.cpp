#include <gtest/gtest.h>

#include "compile/Lexer.hpp"
#include "util/source_file.hpp"

// 注意：不使用 `using namespace aria;`--Windows SDK 的 winnt.h 定义了全局
// `TokenType`，会与 aria::TokenType 在 using-namespace 下产生歧义。故按需
// 显式引入所需符号。
using aria::Error;
using aria::ErrorCode;
using aria::Lexer;
using aria::List;
using aria::SourceFile;
using aria::String;
using aria::StringView;
using aria::Token;
using aria::TokenType;
using aria::UPtr;
using aria::usize;

// Windows SDK winnt.h 有同名全局 TokenType（枚举值）。用别名 TokType 绕开歧义。
using TokType = aria::TokenType;

namespace {
    // 辅助：从源码内容构造一个名为 "t" 的 SourceFile（免文件 IO）。
    SourceFile make_src(StringView content) { return SourceFile{String{"t"}, String{"t"}, String{content}}; }

    // tokenize 结果：需持有 SourceFile，因为 Token::lexeme 是指向其 content 的
    // StringView（生命周期约束：lexeme 不得比 SourceFile 活得久，且 SSO 短串
    // move 后 data 地址会变）。故把 sf 与 tokens 放在堆上对象里返回 unique_ptr--
    // sf 先就位再 tokenize（lexeme 指向该堆上 sf.content），返回时只 move 指针，
    // sf 不再被 move，lexeme 地址稳定。
    struct Lexed {
        SourceFile  sf;
        List<Token> tokens;
    };

    struct LexErrors {
        SourceFile  sf;
        List<Error> errors;
    };

    // 辅助：tokenize 并断言成功，返回堆上 {sf, tokens}。
    UPtr<Lexed> lex_ok(StringView content) {
        auto lexed = std::make_unique<Lexed>();
        lexed->sf  = make_src(content); // sf 就位（此后不再 move）
        Lexer lexer;
        auto  result = lexer.tokenize(lexed->sf);
        EXPECT_TRUE(result.has_value()) << "期望 tokenize 成功";
        lexed->tokens = result.has_value() ? std::move(result.value()) : List<Token>{};
        return lexed;
    }

    // 辅助：tokenize 并断言失败，返回堆上 {sf, errors}。
    UPtr<LexErrors> lex_err(StringView content) {
        auto lexed = std::make_unique<LexErrors>();
        lexed->sf  = make_src(content);
        Lexer lexer;
        auto  result = lexer.tokenize(lexed->sf);
        EXPECT_FALSE(result.has_value()) << "期望 tokenize 失败";
        lexed->errors = result.has_value() ? List<Error>{} : std::move(result.error());
        return lexed;
    }
} // namespace

// ---------------------------------------------------------------------------
// 关键字
// ---------------------------------------------------------------------------

TEST(LexerKeyword, AllKeywords) {
    // 关键字 23 个（finally 已裁撤，现为普通 identifier）
    const String        src      = "fun def var if else while for in break continue return import as "
                                   "try catch throw print nil true false this super match";
    const List<TokType> expected = {TokType::Fun,    TokType::Def,    TokType::Var,   TokType::If,    TokType::Else,
                                    TokType::While,  TokType::For,    TokType::In,    TokType::Break, TokType::Continue,
                                    TokType::Return, TokType::Import, TokType::As,    TokType::Try,   TokType::Catch,
                                    TokType::Throw,  TokType::Print,  TokType::Nil,   TokType::True,  TokType::False,
                                    TokType::This,   TokType::Super,  TokType::Match, TokType::Eof};
    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

TEST(LexerKeyword, FormerLogicalKeywordsAreIdentifiers) {
    // and/or/not 作普通 identifier
    const String        src      = "and or not";
    const List<TokType> expected = {TokType::Identifier, TokType::Identifier, TokType::Identifier, TokType::Eof};
    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

TEST(LexerKeyword, FinallyIsIdentifierAfterRemoval) {
    // finally 回归普通 identifier
    const String        src      = "finally";
    const List<TokType> expected = {TokType::Identifier, TokType::Eof};
    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

// ---------------------------------------------------------------------------
// 运算符（最长匹配）
// ---------------------------------------------------------------------------

TEST(LexerOperator, LongestMatch) {
    const String        src      = "+ += ++ - -= -- * *= / /= % %= = == => ! != > >= < <= && || .. ... .";
    const List<TokType> expected = {
            TokType::Plus,       TokType::PlusEqual,    TokType::PlusPlus,  TokType::Minus,        TokType::MinusEqual,
            TokType::MinusMinus, TokType::Star,         TokType::StarEqual, TokType::Slash,        TokType::SlashEqual,
            TokType::Percent,    TokType::PercentEqual, TokType::Equal,     TokType::EqualEqual,   TokType::FatArrow,
            TokType::Bang,       TokType::BangEqual,    TokType::Greater,   TokType::GreaterEqual, TokType::Less,
            TokType::LessEqual,  TokType::AndAnd,       TokType::OrOr,      TokType::DotDot,       TokType::DotDotDot,
            TokType::Dot,        TokType::Eof};
    const auto  lexed  = lex_ok(src);
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]) << "at token " << i;
    }
}

TEST(LexerOperator, TripleEqual) {
    // === / !== 最长匹配(三字符优先于 ==/!=;哨兵 \0 保证 peek(2) 越界安全)
    const String        src      = "a === b !== c";
    const List<TokType> expected = {TokType::Identifier,     TokType::EqualEqualEqual, TokType::Identifier,
                                    TokType::BangEqualEqual, TokType::Identifier,      TokType::Eof};
    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]) << "at token " << i;
    }
}

TEST(LexerOperator, RangeTokens) {
    // .. 与 ... 最长匹配；1..10 不被当浮点（. 后非数字不消费）
    const String        src      = "1..10 a...b";
    const List<TokType> expected = {TokType::Integer,   TokType::DotDot,     TokType::Integer, TokType::Identifier,
                                    TokType::DotDotDot, TokType::Identifier, TokType::Eof};
    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

// ---------------------------------------------------------------------------
// 标点
// ---------------------------------------------------------------------------

TEST(LexerPunct, Brackets) {
    const auto          lexed    = lex_ok("( ) { } [ ] , : ; .");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::LeftParen,  TokType::RightParen,  TokType::LeftBrace,
                                    TokType::RightBrace, TokType::LeftBracket, TokType::RightBracket,
                                    TokType::Comma,      TokType::Colon,       TokType::Semicolon,
                                    TokType::Dot,        TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

// ---------------------------------------------------------------------------
// Identifier / Underscore
// ---------------------------------------------------------------------------

TEST(LexerIdentifier, Basic) {
    const auto  lexed  = lex_ok("foo _foo foo123 bar_baz");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"foo"});
    EXPECT_EQ(tokens[1].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[1].lexeme(), StringView{"_foo"});
    EXPECT_EQ(tokens[2].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[3].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[4].type(), TokenType::Eof);
}

TEST(LexerIdentifier, LoneUnderscore) {
    // 内联以排除 helper 的生命周期干扰
    SourceFile sf{String{"t"}, String{"t"}, String{"_"}};
    Lexer      lexer;
    auto       result = lexer.tokenize(sf);
    ASSERT_TRUE(result.has_value());
    const auto& tokens = result.value();
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::Underscore);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"_"});
}

TEST(LexerIdentifier, Unicode) {
    // 中文标识符「计数」
    const auto  lexed  = lex_ok("\xE8\xAE\xA1\xE6\x95\xB0");
    const auto& tokens = lexed->tokens; // 计数
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"\xE8\xAE\xA1\xE6\x95\xB0"});
}

// ---------------------------------------------------------------------------
// Integer
// ---------------------------------------------------------------------------

TEST(LexerInteger, Decimal) {
    const auto  lexed  = lex_ok("0 42 1_000");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type(), TokenType::Integer);
    EXPECT_EQ(tokens[0].int_value(), 0);
    EXPECT_EQ(tokens[1].int_value(), 42);
    EXPECT_EQ(tokens[2].int_value(), 1000);
}

TEST(LexerFloat, ScientificNotation) {
    // 含 e/E 指数一律作 float（科学计数法），不再走 int
    const auto  lexed  = lex_ok("1e2 1000e-1 0e-5 1e-1 1.5e2");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 6u);
    EXPECT_EQ(tokens[0].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[0].float_value(), 100.0); // 1e2
    EXPECT_EQ(tokens[1].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[1].float_value(), 100.0); // 1000e-1
    EXPECT_EQ(tokens[2].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[2].float_value(), 0.0); // 0e-5
    EXPECT_EQ(tokens[3].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[3].float_value(), 0.1); // 1e-1
    EXPECT_EQ(tokens[4].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[4].float_value(), 150.0); // 1.5e2
}

TEST(LexerInteger, Radix) {
    const auto  lexed  = lex_ok("0b101 0o17 0xFF 0x1e5");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].int_value(), 5);
    EXPECT_EQ(tokens[1].int_value(), 15);
    EXPECT_EQ(tokens[2].int_value(), 255);
    EXPECT_EQ(tokens[3].int_value(), 485); // 0x1e5 = 1*256 + 14*16 + 5
}

TEST(LexerInteger, RadixUnderscores) {
    // 进制数字中允许 _（须在数字之间）
    const auto  lexed  = lex_ok("0b1_0 0o7_7 0xFF_FF 0x1_e5");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 5u);
    EXPECT_EQ(tokens[0].int_value(), 2);     // 0b1_0 = 2
    EXPECT_EQ(tokens[1].int_value(), 63);    // 0o7_7 = 63
    EXPECT_EQ(tokens[2].int_value(), 65535); // 0xFF_FF
    EXPECT_EQ(tokens[3].int_value(), 485);   // 0x1_e5
}

TEST(LexerInteger, RadixPrefixOnly) {
    // 仅前缀无数字 -> InvalidNumber（断言首字符须为进制数字）
    const auto  lexed = lex_err("0x");
    const auto& errs  = lexed->errors;
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs[0].code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixPrefixThenNonDigit) {
    // 前缀后跟非法字符（如 G 不属 hex）-> InvalidNumber
    const auto  lexed = lex_err("0xG");
    const auto& errs  = lexed->errors;
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs[0].code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixLeadingUnderscore) {
    // 前缀后紧跟 _（首字符须是数字）-> InvalidNumber
    const auto  lexed = lex_err("0x_1");
    const auto& errs  = lexed->errors;
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs[0].code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixTrailingUnderscore) {
    // 数字以 _ 结尾 -> 下划线位置非法 -> InvalidNumber
    const auto  lexed = lex_err("0xFF_");
    const auto& errs  = lexed->errors;
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs[0].code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixOutOfRangeDigit) {
    // 超出进制范围的数字：0b2（二进制无 2）、0o9（八进制无 9）-> InvalidNumber
    const auto  errors = lex_err("0b2");
    const auto& errs   = errors->errors;
    ASSERT_FALSE(errs.empty());
    EXPECT_EQ(errs[0].code(), ErrorCode::InvalidNumber);
    const auto  lexed2 = lex_err("0o9");
    const auto& errs2  = lexed2->errors;
    ASSERT_FALSE(errs2.empty());
    EXPECT_EQ(errs2[0].code(), ErrorCode::InvalidNumber);
}

// ---------------------------------------------------------------------------
// Float
// ---------------------------------------------------------------------------

TEST(LexerFloat, Forms) {
    // 小数点两侧必须各有数字（禁止 .5 / 5. 这类不完整形式）
    const auto  lexed  = lex_ok("1.5 1.5e2 1.5e-3");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u); // 3 个 Float + Eof
    EXPECT_EQ(tokens[0].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[0].float_value(), 1.5);
    EXPECT_EQ(tokens[1].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[1].float_value(), 150.0); // 1.5e2
    EXPECT_EQ(tokens[2].type(), TokenType::Float);
    EXPECT_DOUBLE_EQ(tokens[2].float_value(), 0.0015); // 1.5e-3
}

TEST(LexerFloat, DotNotConsumedForField) {
    // obj.field：identifier 后的 . 不属数字字面量（identifier 走标识符分支），
    // 故 . 留给 Dot token
    const auto  lexed  = lex_ok("obj.field");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u); // Identifier Dot Identifier Eof
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[1].type(), TokenType::Dot);
    EXPECT_EQ(tokens[2].type(), TokenType::Identifier);
}

TEST(LexerFloat, DigitDotNotConsumed) {
    // 1.foo：. 后非数字，. 不再被贪心消费为 float 1.，而是留给 Dot token。
    // 数字作 Int(1)，.foo 的字段访问语义由 parser 决定（lexer 只负责切词）。
    const auto  lexed  = lex_ok("1.foo");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u); // Int Dot Identifier Eof
    EXPECT_EQ(tokens[0].type(), TokenType::Integer);
    EXPECT_EQ(tokens[0].int_value(), 1);
    EXPECT_EQ(tokens[1].type(), TokenType::Dot);
    EXPECT_EQ(tokens[2].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[2].lexeme(), StringView{"foo"});
}

TEST(LexerFloat, IncompleteFormsRejected) {
    // 禁止不完整浮点字面量：.5 / 5. 不再作为单个 float token。
    // .5 -> Dot + Int(5)；5. -> Int(5) + Dot（. 留给 Dot，由 parser 拒绝）。
    {
        const auto  lexed  = lex_ok(".5");
        const auto& tokens = lexed->tokens;
        ASSERT_EQ(tokens.size(), 3u); // Dot Int Eof
        EXPECT_EQ(tokens[0].type(), TokenType::Dot);
        EXPECT_EQ(tokens[1].type(), TokenType::Integer);
        EXPECT_EQ(tokens[1].int_value(), 5);
    }
    {
        const auto  lexed  = lex_ok("5.");
        const auto& tokens = lexed->tokens;
        ASSERT_EQ(tokens.size(), 3u); // Int Dot Eof
        EXPECT_EQ(tokens[0].type(), TokenType::Integer);
        EXPECT_EQ(tokens[0].int_value(), 5);
        EXPECT_EQ(tokens[1].type(), TokenType::Dot);
    }
}

// ---------------------------------------------------------------------------
// String（plainString）
// ---------------------------------------------------------------------------

TEST(LexerString, Basic) {
    const auto  lexed  = lex_ok("\"hello\" 'world'");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 3u);
    EXPECT_EQ(tokens[0].type(), TokenType::String);
    EXPECT_EQ(tokens[0].string_value(), StringView{"hello"});
    EXPECT_EQ(tokens[0].lexeme(), StringView{"\"hello\""});
    EXPECT_EQ(tokens[1].type(), TokenType::String);
    EXPECT_EQ(tokens[1].string_value(), StringView{"world"});
}

TEST(LexerString, Escapes) {
    const auto       lexed  = lex_ok("\"a\\nb\\tc\\rd\\\\e\\0\"");
    const auto&      tokens = lexed->tokens;
    const StringView expected{"a\nb\tc\rd\\e\0", 10};
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].string_value(), expected);
}

TEST(LexerString, UnicodeEscape) {
    const auto  lexed  = lex_ok("\"\\u{4e2d}\"");
    const auto& tokens = lexed->tokens; // 中
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].string_value(), StringView{"\xE4\xB8\xAD"});
}

TEST(LexerString, UnicodeEscapeErrors) {
    // \u 缺少 { -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u41\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{} 空 hex -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{12g} 含非 hex 字符 -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{12g}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{110000} 码点超 0x10FFFF -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{110000}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{d800} 代理区码点 -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{d800}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{1_2} 含 _（hex+ 不允许 _）-> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{1_2}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
    // \u{G} 纯非 hex 字符 -> InvalidEscape
    {
        const auto  lexed = lex_err("\"\\u{G}\"");
        const auto& errs  = lexed->errors;
        ASSERT_FALSE(errs.empty());
        EXPECT_EQ(errs[0].code(), ErrorCode::InvalidEscape);
    }
}

TEST(LexerString, EmptyAndQuoteEscape) {
    const auto  lexed  = lex_ok("\"\" \"a\\\"b\" 'x\\'y'");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].string_value(), StringView{""});
    EXPECT_EQ(tokens[1].string_value(), StringView{"a\"b"});
    EXPECT_EQ(tokens[2].string_value(), StringView{"x'y"});
}

TEST(LexerString, PlainFIsIdentifier) {
    // f 是普通 identifier（无插值字符串：f"..." 不作特殊处理）
    const auto  lexed  = lex_ok("f");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"f"});
}

TEST(LexerString, FPrefixIsNotInterpolation) {
    // 无插值字符串：f"..." 切成 Identifier(f) + String("...") 两个 token
    const auto  lexed  = lex_ok("f\"x\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 3u); // Identifier String Eof
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"f"});
    EXPECT_EQ(tokens[1].type(), TokenType::String);
    EXPECT_EQ(tokens[1].string_value(), StringView{"x"});
}

// ---------------------------------------------------------------------------
// 注释与空白
// ---------------------------------------------------------------------------

TEST(LexerTrivia, CommentsAndWhitespace) {
    const auto  lexed  = lex_ok("// line comment\n # hash comment\n  42  \n");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::Integer);
    EXPECT_EQ(tokens[0].int_value(), 42);
}

// ---------------------------------------------------------------------------
// loc 精确性
// ---------------------------------------------------------------------------

TEST(LexerLoc, Precise) {
    const auto  lexed  = lex_ok("ab 12");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 3u);
    // "ab" 起于第 1 行第 1 列；"12" 起于第 1 行第 4 列
    EXPECT_EQ(tokens[0].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[0].loc().line_col().col, 1u);
    EXPECT_EQ(tokens[1].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[1].loc().line_col().col, 4u);
}

// ---------------------------------------------------------------------------
// 边界
// ---------------------------------------------------------------------------

TEST(LexerEdge, EmptyFile) {
    const auto  lexed  = lex_ok("");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), TokenType::Eof);
}

TEST(LexerEdge, OnlyTrivia) {
    const auto  lexed  = lex_ok("  // comment\n  \n");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens[0].type(), TokenType::Eof);
}

// ---------------------------------------------------------------------------
// 错误：6 个错误码
// ---------------------------------------------------------------------------

TEST(LexerError, UnterminatedString) {
    // EOF 前未闭合
    const auto  lexed  = lex_err("\"abc");
    const auto& errors = lexed->errors;
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::UnterminatedString);

    // 裸换行
    const auto  lexed2  = lex_err("\"abc\ndef\"");
    const auto& errors2 = lexed2->errors;
    ASSERT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code(), ErrorCode::UnterminatedString);
}

TEST(LexerError, InvalidEscape) {
    const auto  lexed  = lex_err("\"\\x41\"");
    const auto& errors = lexed->errors; // 无 \x
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::InvalidEscape);
}

TEST(LexerError, InvalidNumber) {
    const auto  lexed  = lex_err("0x");
    const auto& errors = lexed->errors;
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::InvalidNumber);
}

TEST(LexerError, InvalidCharacter) {
    const auto  lexed  = lex_err("@");
    const auto& errors = lexed->errors;
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::InvalidCharacter);
}

TEST(LexerError, LoneAmpersand) {
    const auto  lexed  = lex_err("&");
    const auto& errors = lexed->errors;
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::InvalidCharacter);
}

// ---------------------------------------------------------------------------
// 错误恢复：可恢复错误后继续扫描
// ---------------------------------------------------------------------------

TEST(LexerRecovery, ContinueAfterRecoverable) {
    // @ 是 InvalidCharacter（可恢复），应继续扫到 42
    SourceFile sf = make_src("@ 42");
    Lexer      lexer;
    auto       result = lexer.tokenize(sf);
    ASSERT_FALSE(result.has_value());
    const auto& errors = result.error();
    ASSERT_FALSE(errors.empty());
    EXPECT_EQ(errors[0].code(), ErrorCode::InvalidCharacter);
}

TEST(LexerRecovery, MultipleErrorsCollected) {
    // 两个非法字符 @ 和 ?
    SourceFile sf = make_src("@ ?");
    Lexer      lexer;
    auto       result = lexer.tokenize(sf);
    ASSERT_FALSE(result.has_value());
    EXPECT_GE(result.error().size(), 2u);
}

TEST(LexerRecovery, UnterminatedStringContinuesScanning) {
    // 未闭合串遇裸换行：记 UnterminatedString（跨行）后跨过换行继续扫，收集到后续 @ 的 InvalidCharacter。
    SourceFile sf = make_src("\"abc\n@");
    Lexer      lexer;
    auto       result = lexer.tokenize(sf);
    ASSERT_FALSE(result.has_value());
    const auto& errors = result.error();
    ASSERT_GE(errors.size(), 2u);
    EXPECT_EQ(errors[0].code(), ErrorCode::UnterminatedString);
    bool has_invalid_char = false;
    for (const auto& e: errors) {
        if (e.code() == ErrorCode::InvalidCharacter) {
            has_invalid_char = true;
            break;
        }
    }
    EXPECT_TRUE(has_invalid_char) << "应在未闭合串后继续扫并收集到 InvalidCharacter";
}

TEST(LexerRecovery, MaxErrorsCapStopsScan) {
    // 超过错误上限的输入应停止扫描，错误数 <= 上限（不无限增长）。
    // 构造大量非法字符；lexer 达 kMaxErrors(32) 即停。
    String src;
    for (usize i = 0; i < 100; ++i) {
        src += "@ ";
    }
    SourceFile sf = make_src(src);
    Lexer      lexer;
    auto       result = lexer.tokenize(sf);
    ASSERT_FALSE(result.has_value());
    // 错误数受上限约束（不应到 100）
    EXPECT_LE(result.error().size(), 64u); // 上限 32，留余量（达上限后不再记账）
}
