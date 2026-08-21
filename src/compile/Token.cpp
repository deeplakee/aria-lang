#include "Token.hpp"
#include <format>

namespace aria {

    namespace {
        // 关键字表：lexeme -> TokenType。与文法「关键字」清单（grammar.txt 末尾）一致，
        // 大小写敏感。逻辑运算符为 &&/||/!（AndAnd/OrOr/Bang），无 and/or/not 关键字别名。
        struct KeywordEntry {
            StringView lexeme;
            TokenType  type;
        };

        // 按字母序，便于人工核对；lookup_keyword 顺序扫描即可（关键字数量少）。
        // 关键字均为 ASCII，使用普通字符串字面量（u8"..." 会得到 char8_t[]，
        // 无法构造 string_view<char>）。
        constexpr KeywordEntry kKeywords[] = {
                {"as", TokenType::As},           {"break", TokenType::Break},
                {"catch", TokenType::Catch},     {"continue", TokenType::Continue},
                {"def", TokenType::Def},         {"else", TokenType::Else},
                {"finally", TokenType::Finally}, {"for", TokenType::For},
                {"fun", TokenType::Fun},         {"if", TokenType::If},
                {"import", TokenType::Import},   {"in", TokenType::In},
                {"match", TokenType::Match},     {"nil", TokenType::Nil},
                {"print", TokenType::Print},     {"return", TokenType::Return},
                {"super", TokenType::Super},     {"this", TokenType::This},
                {"throw", TokenType::Throw},     {"try", TokenType::Try},
                {"var", TokenType::Var},         {"while", TokenType::While},
                {"true", TokenType::True},       {"false", TokenType::False},
        };
    } // namespace

    Opt<TokenType> lookup_keyword(const StringView lexeme) noexcept {
        // 结构化绑定拆出 KeywordEntry 的 (lexeme, type)；
        // 绑定名取 word 而非 lexeme，避免遮蔽外层参数 lexeme 导致自比恒真。
        for (const auto& [word, type]: kKeywords) {
            if (word == lexeme) {
                return type;
            }
        }
        return std::nullopt;
    }

    StringView token_type_name(const TokenType type) noexcept {
        switch (type) {
            case TokenType::Eof:
                return "Eof";
            case TokenType::Error:
                return "Error";
            case TokenType::Integer:
                return "Integer";
            case TokenType::Float:
                return "Float";
            case TokenType::String:
                return "String";
            case TokenType::Identifier:
                return "Identifier";
            case TokenType::Underscore:
                return "Underscore";
            case TokenType::Fun:
                return "Fun";
            case TokenType::Def:
                return "Def";
            case TokenType::Var:
                return "Var";
            case TokenType::If:
                return "If";
            case TokenType::Else:
                return "Else";
            case TokenType::While:
                return "While";
            case TokenType::For:
                return "For";
            case TokenType::In:
                return "In";
            case TokenType::Break:
                return "Break";
            case TokenType::Continue:
                return "Continue";
            case TokenType::Return:
                return "Return";
            case TokenType::Import:
                return "Import";
            case TokenType::As:
                return "As";
            case TokenType::Try:
                return "Try";
            case TokenType::Catch:
                return "Catch";
            case TokenType::Finally:
                return "Finally";
            case TokenType::Throw:
                return "Throw";
            case TokenType::Print:
                return "Print";
            case TokenType::Nil:
                return "Nil";
            case TokenType::True:
                return "True";
            case TokenType::False:
                return "False";
            case TokenType::This:
                return "This";
            case TokenType::Super:
                return "Super";
            case TokenType::Match:
                return "Match";
            case TokenType::Plus:
                return "Plus";
            case TokenType::Minus:
                return "Minus";
            case TokenType::Star:
                return "Star";
            case TokenType::Slash:
                return "Slash";
            case TokenType::Percent:
                return "Percent";
            case TokenType::PlusEqual:
                return "PlusEqual";
            case TokenType::MinusEqual:
                return "MinusEqual";
            case TokenType::StarEqual:
                return "StarEqual";
            case TokenType::SlashEqual:
                return "SlashEqual";
            case TokenType::PercentEqual:
                return "PercentEqual";
            case TokenType::Equal:
                return "Equal";
            case TokenType::EqualEqual:
                return "EqualEqual";
            case TokenType::EqualEqualEqual:
                return "EqualEqualEqual";
            case TokenType::BangEqual:
                return "BangEqual";
            case TokenType::BangEqualEqual:
                return "BangEqualEqual";
            case TokenType::Bang:
                return "Bang";
            case TokenType::Greater:
                return "Greater";
            case TokenType::GreaterEqual:
                return "GreaterEqual";
            case TokenType::Less:
                return "Less";
            case TokenType::LessEqual:
                return "LessEqual";
            case TokenType::AndAnd:
                return "AndAnd";
            case TokenType::OrOr:
                return "OrOr";
            case TokenType::PlusPlus:
                return "PlusPlus";
            case TokenType::MinusMinus:
                return "MinusMinus";
            case TokenType::FatArrow:
                return "FatArrow";
            case TokenType::LeftParen:
                return "LeftParen";
            case TokenType::RightParen:
                return "RightParen";
            case TokenType::LeftBrace:
                return "LeftBrace";
            case TokenType::RightBrace:
                return "RightBrace";
            case TokenType::LeftBracket:
                return "LeftBracket";
            case TokenType::RightBracket:
                return "RightBracket";
            case TokenType::Comma:
                return "Comma";
            case TokenType::Colon:
                return "Colon";
            case TokenType::Semicolon:
                return "Semicolon";
            case TokenType::Dot:
                return "Dot";
            case TokenType::DotDot:
                return "DotDot";
            case TokenType::DotDotDot:
                return "DotDotDot";
        }
        return "Unknown";
    }

    StringView Token::error_message() const noexcept {
        if (type_ != TokenType::Error) {
            return {};
        }
        const auto msg_ptr = std::get_if<String>(&value_);
        return msg_ptr ? StringView{*msg_ptr} : StringView{};
    }

    i64 Token::int_value() const noexcept {
        if (type_ != TokenType::Integer) {
            return 0;
        }
        const auto ival_ptr = std::get_if<i64>(&value_);
        return ival_ptr ? *ival_ptr : 0;
    }

    f64 Token::float_value() const noexcept {
        if (type_ != TokenType::Float) {
            return 0.0;
        }
        const auto fval_ptr = std::get_if<f64>(&value_);
        return fval_ptr ? *fval_ptr : 0.0;
    }

    StringView Token::string_value() const noexcept {
        if (type_ != TokenType::String) {
            return {};
        }
        const auto* sval_ptr = std::get_if<String>(&value_);
        return sval_ptr ? StringView{*sval_ptr} : StringView{};
    }

    bool Token::is_literal() const noexcept {
        // 词法层「值/名」分类：直接表示一个值或名字的 token（非符号/关键字/控制流），
        // 作为 token 层四分类之一（与 is_keyword/is_operator/is_punctuation 正交并互补）。
        // 注意：此处的 literal 是词法概念，与文法产生式 literal（编译期常量，仅
        // number/plainString/true/false/nil，用作 mapEntry 键）不同--后者由 parser 按需判断。
        // 故 Identifier、Underscore 在本分类中算入，尽管它们不属于文法 literal。
        switch (type_) {
            case TokenType::Integer:
            case TokenType::Float:
            case TokenType::String:
            case TokenType::Identifier:
            case TokenType::Underscore:
                return true;
            default:
                return false;
        }
    }

    bool Token::is_keyword() const noexcept {
        switch (type_) {
            case TokenType::Fun:
            case TokenType::Def:
            case TokenType::Var:
            case TokenType::If:
            case TokenType::Else:
            case TokenType::While:
            case TokenType::For:
            case TokenType::In:
            case TokenType::Break:
            case TokenType::Continue:
            case TokenType::Return:
            case TokenType::Import:
            case TokenType::As:
            case TokenType::Try:
            case TokenType::Catch:
            case TokenType::Finally:
            case TokenType::Throw:
            case TokenType::Print:
            case TokenType::Nil:
            case TokenType::True:
            case TokenType::False:
            case TokenType::This:
            case TokenType::Super:
            case TokenType::Match:
                return true;
            default:
                return false;
        }
    }

    bool Token::is_operator() const noexcept {
        switch (type_) {
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Star:
            case TokenType::Slash:
            case TokenType::Percent:
            case TokenType::PlusEqual:
            case TokenType::MinusEqual:
            case TokenType::StarEqual:
            case TokenType::SlashEqual:
            case TokenType::PercentEqual:
            case TokenType::Equal:
            case TokenType::EqualEqual:
            case TokenType::EqualEqualEqual:
            case TokenType::BangEqual:
            case TokenType::BangEqualEqual:
            case TokenType::Bang:
            case TokenType::Greater:
            case TokenType::GreaterEqual:
            case TokenType::Less:
            case TokenType::LessEqual:
            case TokenType::AndAnd:
            case TokenType::OrOr:
            case TokenType::PlusPlus:
            case TokenType::MinusMinus:
            case TokenType::FatArrow:
            case TokenType::DotDot:
                return true;
            default:
                return false;
        }
    }

    bool Token::is_punctuation() const noexcept {
        switch (type_) {
            case TokenType::LeftParen:
            case TokenType::RightParen:
            case TokenType::LeftBrace:
            case TokenType::RightBrace:
            case TokenType::LeftBracket:
            case TokenType::RightBracket:
            case TokenType::Comma:
            case TokenType::Colon:
            case TokenType::Semicolon:
            case TokenType::Dot:
            case TokenType::DotDotDot:
                return true;
            default:
                return false;
        }
    }

    String Token::to_string() const {
        // 类型名 + 可选 lexeme（'...'）。lexeme 为空时省略引号段。
        String lexeme_info = lexeme_.empty() ? std::format("{:<13}", token_type_name(type_))
                                             : std::format("{:<13} '{}'", token_type_name(type_), lexeme_);
        // 字面量附带解析值，便于调试；Error 附带消息
        switch (type_) {
            case TokenType::Integer:
                return std::format("{} = {}", lexeme_info, int_value());
            case TokenType::Float:
                return std::format("{} = {}", lexeme_info, float_value());
            case TokenType::Error:
                return std::format("{}: {}", lexeme_info, error_message());
            default:
                return lexeme_info;
        }
    }

} // namespace aria
