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

        // lookup_keyword 顺序扫描即可（关键字数量少）。
        // 关键字均为 ASCII，使用普通字符串字面量（u8"..." 会得到 char8_t[]，
        // 无法构造 string_view<char>）。
        constexpr KeywordEntry kKeywords[] = {
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
    } // namespace

    Opt<TokenType> lookup_keyword(const StringView lexeme) noexcept {
        // 绑定名取 word 而非 lexeme，避免遮蔽外层参数 lexeme 导致自比恒真。
        for (const auto& [word, type]: kKeywords) {
            if (word == lexeme) {
                return type;
            }
        }
        return std::nullopt;
    }

    StringView to_string(const TokenType type) noexcept {
        switch (type) {
            case TokenType::Eof:
                return "Eof";
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
        auto sval_ptr = std::get_if<String>(&value_);
        return sval_ptr ? StringView{*sval_ptr} : StringView{};
    }

    String Token::to_string() const {
        // 类作用域内 to_string 查到成员自身即停，须限定到命名空间作用域的自由函数（同 Object::type_name）。
        String lexeme_info = lexeme_.empty() ? std::format("{:<13}", aria::to_string(type_))
                                             : std::format("{:<13} '{}'", aria::to_string(type_), lexeme_);
        // 字面量附带解析值，便于调试
        switch (type_) {
            case TokenType::Integer:
                return std::format("{} = {}", lexeme_info, int_value());
            case TokenType::Float:
                return std::format("{} = {}", lexeme_info, float_value());
            default:
                return lexeme_info;
        }
    }

} // namespace aria
