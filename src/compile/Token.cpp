#include "Token.hpp"
#include <format>

namespace aria {

    i64 Token::int_value() const noexcept { return type_ == TokenType::Integer ? value_.int_ : 0; }

    f64 Token::float_value() const noexcept { return type_ == TokenType::Float ? value_.float_ : 0.0; }

    StringView Token::string_value() const noexcept { return type_ == TokenType::String ? value_.str_ : StringView{}; }

    StringView Token::interp_value() const noexcept {
        switch (type_) {
            case TokenType::InterpStart:
            case TokenType::InterpMiddle:
            case TokenType::InterpEnd:
                return value_.str_;
            default:
                return {};
        }
    }

    String Token::to_string() const {
        // 类作用域内 to_string 查到成员自身即停，须限定到命名空间作用域的自由函数。
        String lexeme_info = lexeme().empty() ? std::format("{:<13}", aria::to_string(type_))
                                              : std::format("{:<13} '{}'", aria::to_string(type_), lexeme());
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
