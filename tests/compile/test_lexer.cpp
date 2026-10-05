#include <gtest/gtest.h>

#include <tuple>

#include "compile/Lexer.hpp"
#include "util/source_file.hpp"
#include "util/str.hpp"

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
    SourceFile make_src(const StringView content) { return SourceFile{String{"t"}, String{"t"}, String{content}}; }

    // tokenize 结果：需持有 SourceFile，因为 Token::lexeme 是指向其 content 的
    // StringView（生命周期约束：lexeme 不得比 SourceFile 活得久，且 SSO 短串
    // move 后 data 地址会变）。故把 sf 与 tokens 放在堆上对象里返回 unique_ptr--
    // sf 先就位再 tokenize（lexeme 指向该堆上 sf.content），返回时只 move 指针，
    // sf 不再被 move，lexeme 地址稳定。
    struct Lexed {
        SourceFile  sf;
        List<Token> tokens; // token 的 lexeme/内层视图借自 sf 的源缓冲，sf 须与 token 表同寿命
    };

    // 辅助：tokenize 并断言成功，返回堆上 {sf, tokens}。
    UPtr<Lexed> lex_ok(const StringView content) {
        auto lexed  = std::make_unique<Lexed>();
        lexed->sf   = make_src(content); // sf 就位（此后不再 move）
        auto result = Lexer::tokenize(lexed->sf);
        EXPECT_TRUE(result.has_value()) << "期望 tokenize 成功";
        lexed->tokens = result ? std::move(*result) : List<Token>{};
        return lexed;
    }

    // 辅助：tokenize 并断言失败，返回首错（Error 消息已烘焙，不依赖 sf 存活）。
    Error lex_err(const StringView content) {
        SourceFile sf     = make_src(content);
        auto       result = Lexer::tokenize(sf);
        EXPECT_FALSE(result.has_value()) << "期望 tokenize 失败";
        if (result) {
            return Error::from_detail(ErrorCode::Unreachable, "tokenize unexpectedly succeeded");
        }
        return std::move(result.error());
    }

    // 按消费端形态展开字面 token：内层原文视图 + 词法期记账长度 + 转义标记，经解码器取回内容串
    // （与 CodeGen 的转义臂同一管线，替代旧 string_value/interp_value 断言面）。
    String decoded(const Token& token) {
        String out;
        out.resize(token.shape().decoded_len);
        std::ignore = aria::str::decode_string_content(token.string_value(), out.data());
        return out;
    }
} // namespace

// ---------------------------------------------------------------------------
// 关键字
// ---------------------------------------------------------------------------

TEST(LexerKeyword, AllKeywords) {
    // 关键字 22 个（finally 非关键字，按普通 identifier 处理）
    const String        src      = "fun def var if else while for in break continue return import as "
                                   "try catch throw nil true false this super match";
    const List<TokType> expected = {TokType::Fun,    TokType::Def,    TokType::Var,  TokType::If,    TokType::Else,
                                    TokType::While,  TokType::For,    TokType::In,   TokType::Break, TokType::Continue,
                                    TokType::Return, TokType::Import, TokType::As,   TokType::Try,   TokType::Catch,
                                    TokType::Throw,  TokType::Nil,    TokType::True, TokType::False, TokType::This,
                                    TokType::Super,  TokType::Match,  TokType::Eof};
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
    // finally 是普通 identifier
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
    auto       result = Lexer::tokenize(sf);
    ASSERT_TRUE(result.has_value());
    const auto& tokens = *result;
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
    // 含 e/E 指数一律作 float（科学计数法），不作 int
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
    const auto err = lex_err("0x");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixPrefixThenNonDigit) {
    // 前缀后跟非法字符（如 G 不属 hex）-> InvalidNumber
    const auto err = lex_err("0xG");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixLeadingUnderscore) {
    // 前缀后紧跟 _（首字符须是数字）-> InvalidNumber
    const auto err = lex_err("0x_1");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixTrailingUnderscore) {
    // 数字以 _ 结尾 -> 下划线位置非法 -> InvalidNumber
    const auto err = lex_err("0xFF_");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
}

TEST(LexerInteger, RadixOutOfRangeDigit) {
    // 超出进制范围的数字：0b2（二进制无 2）、0o9（八进制无 9）-> InvalidNumber
    const auto err = lex_err("0b2");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
    const auto err2 = lex_err("0o9");
    EXPECT_EQ(err2.code(), ErrorCode::InvalidNumber);
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
    // 1.foo：. 后非数字，. 不被贪心消费为 float 1.，而是留给 Dot token。
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
    // 禁止不完整浮点字面量：.5 / 5. 不作单个 float token。
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
    EXPECT_EQ(decoded(tokens[0]), "hello");
    EXPECT_FALSE(tokens[0].shape().is_escape);
    EXPECT_EQ(tokens[0].shape().decoded_len, 5u);
    EXPECT_EQ(tokens[0].string_value(), StringView{"hello"});
    EXPECT_EQ(tokens[0].lexeme(), StringView{"\"hello\""});
    EXPECT_EQ(tokens[1].type(), TokenType::String);
    EXPECT_EQ(decoded(tokens[1]), "world");
}

TEST(LexerString, Escapes) {
    const auto  lexed  = lex_ok("\"a\\nb\\tc\\rd\\\\e\\0\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_TRUE(tokens[0].shape().is_escape);
    EXPECT_EQ((tokens[0].string_value()), StringView{"a\\nb\\tc\\rd\\\\e\\0"}); // 内层原文,转义未展开
    EXPECT_EQ(tokens[0].shape().decoded_len, 10u);                              // 展开后 10 字节
    EXPECT_EQ(decoded(tokens[0]), (String{"a\nb\tc\rd\\e\0", 10}));
}

TEST(LexerString, UnicodeEscape) {
    const auto  lexed  = lex_ok("\"\\u{4e2d}\"");
    const auto& tokens = lexed->tokens; // 中
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].shape().decoded_len, 3u); // 3 字节码点
    EXPECT_EQ(decoded(tokens[0]), "\xE4\xB8\xAD");
}

TEST(LexerString, UnicodeEscapeErrors) {
    // \u 缺少 { -> InvalidEscape
    {
        const auto err = lex_err("\"\\u41\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
        EXPECT_NE(err.message().find(":1:2:"), String::npos); // 锚点=反斜杠列（非 u 列）
    }
    // \u{} 空 hex -> InvalidEscape
    {
        const auto err = lex_err("\"\\u{}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
    // \u{12g} 含非 hex 字符 -> InvalidEscape
    {
        const auto err = lex_err("\"\\u{12g}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
    // \u{110000} 码点超 0x10FFFF -> InvalidEscape
    {
        const auto err = lex_err("\"\\u{110000}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
    // \u{d800} 代理区码点 -> InvalidEscape
    {
        const auto err = lex_err("\"\\u{d800}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
    // \u{1_2} 含 _（hex+ 不允许 _）-> InvalidEscape
    {
        const auto err = lex_err("\"\\u{1_2}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
    // \u{G} 纯非 hex 字符 -> InvalidEscape
    {
        const auto err = lex_err("\"\\u{G}\"");
        EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
    }
}

TEST(LexerString, EmptyAndQuoteEscape) {
    const auto  lexed  = lex_ok("\"\" \"a\\\"b\" 'x\\'y'");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].shape().decoded_len, 0u);
    EXPECT_FALSE(tokens[0].shape().is_escape);
    EXPECT_TRUE(tokens[1].shape().is_escape); // \" 转义
    EXPECT_EQ(decoded(tokens[0]), "");
    EXPECT_EQ(decoded(tokens[1]), "a\"b");
    EXPECT_EQ(decoded(tokens[2]), "x'y");
}

TEST(LexerString, PlainFIsIdentifier) {
    // f 是普通 identifier（插值无 f 前缀形态：f"..." 不作特殊处理）
    const auto  lexed  = lex_ok("f");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"f"});
}

TEST(LexerString, FPrefixIsNotInterpolation) {
    // 插值无 f 前缀形态：f"..." 切成 Identifier(f) + String("...") 两个 token
    const auto  lexed  = lex_ok("f\"x\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 3u); // Identifier String Eof
    EXPECT_EQ(tokens[0].type(), TokenType::Identifier);
    EXPECT_EQ(tokens[0].lexeme(), StringView{"f"});
    EXPECT_EQ(tokens[1].type(), TokenType::String);
    EXPECT_EQ(decoded(tokens[1]), "x");
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

TEST(LexerLoc, TracksAcrossLinesAndCodepoints) {
    // token 位置由起点偏移派生：换行归位、多字节码点按 1 列计，与 locate 行首计数逐位一致
    const auto  lexed  = lex_ok("ab\n中文 12");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u); // ab / 中文 / 12 / Eof
    EXPECT_EQ(tokens[0].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[0].loc().line_col().col, 1u);
    EXPECT_EQ(tokens[1].loc().line_col().line, 2u); // 中文起于 2 行 1 列
    EXPECT_EQ(tokens[1].loc().line_col().col, 1u);
    EXPECT_EQ(tokens[2].loc().line_col().line, 2u); // 空格后 12 在 2 行 4 列
    EXPECT_EQ(tokens[2].loc().line_col().col, 4u);
    EXPECT_EQ(tokens[3].type(), TokenType::Eof);
}

TEST(LexerLoc, RadixFloatAndCommentLines) {
    // 注释体（含多字节）跨过不影响位置派生：行尾换行即归位；进制/浮点 token 位置仍精确
    const auto  lexed  = lex_ok("0x1F // 中文注释\n1.5e2");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 3u); // 0x1F / 1.5e2 / Eof
    EXPECT_EQ(tokens[0].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[0].loc().line_col().col, 1u);
    EXPECT_EQ(tokens[1].loc().line_col().line, 2u);
    EXPECT_EQ(tokens[1].loc().line_col().col, 1u);
}

TEST(LexerLoc, ExpBacktrackKeepsFollowingTokensAligned) {
    // e 后无数字回退只改游标（位置无其他状态可还原）：后续 token 不串列
    const auto  lexed  = lex_ok("1e 2");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u); // 1 / e / 2 / Eof
    EXPECT_EQ(tokens[0].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[0].loc().line_col().col, 1u);
    EXPECT_EQ(tokens[1].loc().line_col().line, 1u); // 回退后 e 起于 1 行 2 列
    EXPECT_EQ(tokens[1].loc().line_col().col, 2u);
    EXPECT_EQ(tokens[2].loc().line_col().line, 1u);
    EXPECT_EQ(tokens[2].loc().line_col().col, 4u);
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
    const auto err = lex_err("\"abc");
    EXPECT_EQ(err.code(), ErrorCode::UnterminatedString);

    // 裸换行
    const auto err2 = lex_err("\"abc\ndef\"");
    EXPECT_EQ(err2.code(), ErrorCode::UnterminatedString);
}

TEST(LexerError, InvalidEscape) {
    const auto err = lex_err("\"\\x41\""); // 无 \x
    EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
}

TEST(LexerError, InvalidNumber) {
    const auto err = lex_err("0x");
    EXPECT_EQ(err.code(), ErrorCode::InvalidNumber);
}

TEST(LexerError, InvalidCharacter) {
    const auto err = lex_err("@");
    EXPECT_EQ(err.code(), ErrorCode::InvalidCharacter);
}

TEST(LexerError, LoneAmpersand) {
    const auto err = lex_err("&");
    EXPECT_EQ(err.code(), ErrorCode::InvalidCharacter);
}

TEST(LexerError, FirstErrorOnly) {
    // 多个非法字符只报第一个（@ = U+0040；? 在其后，不再报）
    const auto err = lex_err("@ ?");
    EXPECT_EQ(err.code(), ErrorCode::InvalidCharacter);
    EXPECT_NE(err.message().find("U+0040"), String::npos);
}

// ---------------------------------------------------------------------------
// 字符串模板（"...${expr}..."，无前缀；裸 $ 与 { } 皆普通字符，仅 ${ 开档）
// ---------------------------------------------------------------------------

TEST(LexerInterp, BasicSplit) {
    // "a ${x} b" -> Start("a ") x End(" b")
    const auto          lexed    = lex_ok("\"a ${x} b\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(decoded(tokens[0]), "a ");
    EXPECT_EQ(decoded(tokens[2]), " b");
}

TEST(LexerInterp, MultipleHoles) {
    // "a${x}b${y}c" -> Start Middle End 全形态
    const auto          lexed    = lex_ok("\"a${x}b${y}c\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpMiddle,
                                    TokType::Identifier,  TokType::InterpEnd,  TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(decoded(tokens[0]), "a");
    EXPECT_EQ(decoded(tokens[2]), "b");
    EXPECT_EQ(decoded(tokens[4]), "c");
}

TEST(LexerInterp, EmptyEdgeSegments) {
    // 首尾档紧贴引号：空字面段照常产出
    const auto          lexed    = lex_ok("\"${x}\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(tokens[0].shape().decoded_len, 0u); // 空字面段照常产出,展开后零字节
    EXPECT_EQ(tokens[2].shape().decoded_len, 0u);
}

TEST(LexerInterp, ExpressionHoleWithBraces) {
    // 档内花括号配对：map 字面量与 lambda 体的 { } 产 token 不闭档
    const auto          lexed    = lex_ok("\"${ {1:2} }\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::LeftBrace,  TokType::Integer,   TokType::Colon,
                                    TokType::Integer,     TokType::RightBrace, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }

    const auto          lexed2    = lex_ok("\"${fun(n){ n }(1)}\"");
    const auto&         tokens2   = lexed2->tokens;
    const List<TokType> expected2 = {TokType::InterpStart, TokType::Fun,       TokType::LeftParen,  TokType::Identifier,
                                     TokType::RightParen,  TokType::LeftBrace, TokType::Identifier, TokType::RightBrace,
                                     TokType::LeftParen,   TokType::Integer,   TokType::RightParen, TokType::InterpEnd,
                                     TokType::Eof};
    ASSERT_EQ(tokens2.size(), expected2.size());
    for (usize i = 0; i < tokens2.size(); ++i) {
        EXPECT_EQ(tokens2[i].type(), expected2[i]);
    }
}

TEST(LexerInterp, Nested) {
    // 嵌套插值：内层完整产出（Start/Middle/End），外层档深度不受内层 { } 干扰
    const auto          lexed    = lex_ok("\"${ \"in ${x}\" }\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::InterpStart, TokType::Identifier,
                                    TokType::InterpEnd,   TokType::InterpEnd,   TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(decoded(tokens[1]), "in ");
}

TEST(LexerInterp, PlainStringInHole) {
    // 档内普通串：完整 String token，串内 { } 与 $ 不参与档深计数
    const auto          lexed    = lex_ok("\"${ \"a}b\" }\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::String, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(decoded(tokens[1]), "a}b");
}

TEST(LexerInterp, SingleQuoteForm) {
    // '...' 与 "..." 同为模板
    const auto          lexed    = lex_ok("'a ${x}'");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
}

TEST(LexerInterp, NoHoleStaysPlainString) {
    // 无 ${ 的串是普通 String token：裸 $（含后随标识符）不开档
    const auto  lexed  = lex_ok("\"a$x b\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokType::String);
    EXPECT_EQ(decoded(tokens[0]), "a$x b");

    const auto  lexed2  = lex_ok("\"$100 and $\"");
    const auto& tokens2 = lexed2->tokens;
    ASSERT_EQ(tokens2.size(), 2u);
    EXPECT_EQ(tokens2[0].type(), TokType::String);
    EXPECT_EQ(decoded(tokens2[0]), "$100 and $");
}

TEST(LexerInterp, EscapedDollarIsLiteral) {
    // \$ 转义产出字面 ${（裸 $ 无需转义）
    const auto  lexed  = lex_ok("\"a\\${b}\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 2u);
    EXPECT_EQ(tokens[0].type(), TokType::String);
    EXPECT_TRUE(tokens[0].shape().is_escape);
    EXPECT_EQ(decoded(tokens[0]), "a${b}");

    const auto  lexed2  = lex_ok("\"a\\$b\"");
    const auto& tokens2 = lexed2->tokens;
    ASSERT_EQ(tokens2.size(), 2u);
    EXPECT_EQ(tokens2[0].type(), TokType::String);
    EXPECT_EQ(decoded(tokens2[0]), "a$b");
}

TEST(LexerInterp, EscapesInSegment) {
    // 字面段内普通转义照常解析；\$ 转义后段继续，其后 ${ 开档
    const auto          lexed    = lex_ok("\"a\\n\\${ ${x}\"");
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    EXPECT_EQ(decoded(tokens[0]), "a\n${ ");
}

TEST(LexerInterp, BareBracesAreLiteral) {
    // 串文本里裸 { } 无档语义，普通字符
    const auto  lexed  = lex_ok("\"a}b{c ${x}\"");
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 4u);
    EXPECT_EQ(tokens[0].type(), TokType::InterpStart);
    EXPECT_EQ(decoded(tokens[0]), "a}b{c ");
}

TEST(LexerInterp, EscapedBraceIsInvalid) {
    // \{ 不再是转义（{ 本就是普通字符）
    const auto err = lex_err("\"a\\{b\"");
    EXPECT_EQ(err.code(), ErrorCode::InvalidEscape);
}

TEST(LexerInterp, MaxDepthAccepted) {
    // 16 层嵌套合法
    String src;
    for (usize i = 0; i < 16; ++i) {
        src += "\"${";
    }
    src += "x";
    for (usize i = 0; i < 16; ++i) {
        src += "}\"";
    }
    const auto lexed = lex_ok(src);
    EXPECT_EQ(lexed->tokens.back().type(), TokType::Eof);
}

TEST(LexerInterp, DepthExceeded) {
    // 17 层嵌套：InterpDepthExceeded
    String src;
    for (usize i = 0; i < 17; ++i) {
        src += "\"${";
    }
    src += "x";
    for (usize i = 0; i < 17; ++i) {
        src += "}\"";
    }
    const auto err = lex_err(src);
    EXPECT_EQ(err.code(), ErrorCode::InterpDepthExceeded);
}

TEST(LexerInterp, UnterminatedVariants) {
    // 文本态 EOF
    const auto err1 = lex_err("\"abc");
    EXPECT_EQ(err1.code(), ErrorCode::UnterminatedString);

    // 档内 EOF（闭档 } 与闭引号缺失）
    const auto err2 = lex_err("\"a ${x");
    EXPECT_EQ(err2.code(), ErrorCode::UnterminatedString);

    // 闭档后 EOF
    const auto err3 = lex_err("\"a ${x}");
    EXPECT_EQ(err3.code(), ErrorCode::UnterminatedString);

    // 文本态裸换行（不跨行）
    const auto err4 = lex_err("\"a${x}\nb\"");
    EXPECT_EQ(err4.code(), ErrorCode::UnterminatedString);

    // 闭引号被档吞作新串起点（${ 后直撞引号）
    const auto err5 = lex_err("\"abc${\"");
    EXPECT_EQ(err5.code(), ErrorCode::UnterminatedString);
}

TEST(LexerInterp, DollarOutsideStringIsInvalid) {
    // 字符串外的 $ 无语法地位：InvalidCharacter
    const auto err1 = lex_err("$x");
    EXPECT_EQ(err1.code(), ErrorCode::InvalidCharacter);

    const auto err2 = lex_err("$ \"abc\"");
    EXPECT_EQ(err2.code(), ErrorCode::InvalidCharacter);
}

// ---------------------------------------------------------------------------
// 超大字面量端到端钉子（原文只以 lexeme 借自源缓冲,无独立存储层）
// ---------------------------------------------------------------------------

TEST(LexerString, SourceWithHugeLiteral) {
    // 超大字面量（4 MB+）:值借源缓冲存活,内容完整、记账正确
    String src;
    src.reserve(4 * 1024 * 1024 + 256);
    src += "var s0 = \"";
    src.append(4 * 1024 * 1024 + 100, 'a');
    src += "\";\n";
    const auto tail = String(64, 'b');
    src += "var s1 = \"" + tail + "中文转义\\n\";\n";

    const auto  lexed  = lex_ok(src);
    const auto& tokens = lexed->tokens;
    ASSERT_EQ(tokens.size(), 11u); // Var Id Eq Str ; Var Id Eq Str ; Eof
    EXPECT_EQ(tokens[3].type(), TokenType::String);
    const auto expected = String(4 * 1024 * 1024 + 100, 'a');
    EXPECT_EQ(decoded(tokens[3]), expected);
    EXPECT_EQ(tokens[3].shape().decoded_len, expected.size());
    EXPECT_FALSE(tokens[3].shape().is_escape);
    EXPECT_EQ(tokens[8].type(), TokenType::String);
    EXPECT_TRUE(tokens[8].shape().is_escape);
    EXPECT_EQ(decoded(tokens[8]), tail + "中文转义\n");
}

TEST(LexerInterp, HugeSegments) {
    // 两段字面合计超大:段原文借源缓冲各自连续,记账与内容一致
    const auto head = String(2 * 1024 * 1024, 'a');
    const auto tail = String(3 * 1024 * 1024, 'b');
    const auto src  = "\"" + head + "${x}" + tail + "\"";

    const auto          lexed    = lex_ok(src);
    const auto&         tokens   = lexed->tokens;
    const List<TokType> expected = {TokType::InterpStart, TokType::Identifier, TokType::InterpEnd, TokType::Eof};
    ASSERT_EQ(tokens.size(), expected.size());
    for (usize i = 0; i < tokens.size(); ++i) {
        EXPECT_EQ(tokens[i].type(), expected[i]);
    }
    EXPECT_EQ(decoded(tokens[0]), head);
    EXPECT_EQ(tokens[0].shape().decoded_len, head.size());
    EXPECT_EQ(decoded(tokens[2]), tail);
    EXPECT_EQ(tokens[2].shape().decoded_len, tail.size());
}
