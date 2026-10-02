#include "Token.hpp"
#include <format>

namespace aria {

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
        // 类作用域内 to_string 查到成员自身即停，须限定到命名空间作用域的自由函数。
        String lexeme_info = lexeme_.empty() ? std::format("{:<13}", aria::to_string(type_))
                                             : std::format("{:<13} '{}'", aria::to_string(type_), lexeme_);
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
