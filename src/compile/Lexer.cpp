#include "Lexer.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {

        bool is_digit(const char ch) { return ch >= '0' && ch <= '9'; }

        // 引号字符（串字面量的两种定界形态 " 与 '）
        bool is_quote(const char ch) { return ch == '"' || ch == '\''; }

        bool is_radix_digit(const u8 base, const char ch) {
            if (base < 2 || base > 36) {
                return false;
            }
            if (base <= 10) {
                return ch >= '0' && ch < static_cast<char>('0' + base);
            }

            const bool lower = (ch >= 'a' && ch < static_cast<char>('a' + base - 10));
            const bool upper = (ch >= 'A' && ch < static_cast<char>('A' + base - 10));
            return is_digit(ch) || lower || upper;
        }

        // _ 必须位于两个进制数字之间（hex 的 a-f/A-F 也算，故用 is_radix_digit）；lex 已剥进制前缀。
        bool validate_underscores(const StringView lex, const u8 base) {
            const usize n = lex.size();
            for (usize i = 0; i < n; ++i) {
                if (lex[i] != '_') {
                    continue;
                }
                const bool prev_ok = i > 0 && is_radix_digit(base, lex[i - 1]);
                const bool next_ok = i + 1 < n && is_radix_digit(base, lex[i + 1]);
                if (!prev_ok || !next_ok) {
                    return false;
                }
            }
            return true;
        }

        Opt<usize> strip_underscores(const StringView lex, char* out, const usize out_cap) {
            usize out_size = 0;
            for (const char i: lex) {
                if (i == '_') {
                    continue;
                }
                if (out_size >= out_cap) {
                    return std::nullopt;
                }

                out[out_size++] = i;
            }
            return out_size;
        }

        // 解析整数字面量为 i64（lex 不含进制前缀与指数，含指数的走 float）；溢出返 nullopt。
        Opt<i64> parse_int(const StringView lex, const u8 base) {
            char buf[64];
            if (const auto len = strip_underscores(lex, buf, sizeof(buf))) {
                return util::try_parse<i64>(StringView{buf, *len}, base);
            }
            return std::nullopt;
        }

        Opt<f64> parse_float(const StringView lex) {
            char buf[64];
            if (const auto len = strip_underscores(lex, buf, sizeof(buf))) {
                return util::try_parse<f64>(StringView{buf, *len});
            }
            return std::nullopt;
        }
    } // namespace

    Lexer::Lexer(SourceFile& src) noexcept :
        source_{src}, src_{src.content()}, pos_{0}, tokens_{}, errors_{}, is_fatal_{false} {}

    Result<List<Token>, List<Error>> Lexer::tokenize(SourceFile& src) {
        Lexer lexer{src};
        lexer.run();

        if (!lexer.errors_.empty()) {
            return std::unexpected(std::move(lexer.errors_));
        }
        return std::move(lexer.tokens_);
    }

    char Lexer::peek_byte(const u32 ahead) const noexcept {
        const u32 index = pos_ + ahead;
        if (index >= src_.size()) {
            return '\0';
        }
        return src_[index];
    }

    void Lexer::advance(const u32 n) noexcept {
        ASSERT(pos_ + n <= src_.size(), "beyond end of source");
        pos_ += n;
    }

    bool Lexer::is_eof() const noexcept { return pos_ >= src_.size(); }

    void Lexer::error(const ErrorCode code, const StringView msg, const u32 offset) {
        errors_.push_back(Error::from_detail(code, SourceLoc{&source_, offset}, msg));
        if (errors_.size() >= kMaxErrors) {
            is_fatal_ = true;
        }
    }

    void Lexer::run() {
        while (!is_fatal_ && !is_eof()) {
            dispatch_one();
        }

        if (is_fatal_) {
            return; // 致命错误，不补 Eof
        }
        // EOF token 位置 = 内容末尾偏移；行列由 SourceLoc 派生为「下一行第 1 列」（编辑器约定）。
        const u32 eof = src_.size();
        tokens_.push_back(Token{TokenType::Eof, {}, SourceLoc{&source_, eof}});
    }

    // 单步分派：产一个普通 token（或吞一段 trivia）。run 主循环与后续的插值档内扫描共用。
    void Lexer::dispatch_one() {
        const auto cp = utf8::decode_one(src_, pos_).first; // 各分支自行解码推进

        if (utf8::is_whitespace(cp) || is_line_comment_start(cp)) {
            skip_trivia();
            return;
        }

        // 以数字开头才走数字扫描（. 不启动数字--禁 .5 这类不完整浮点，. 留给 Dot token）。
        if (utf8::is_digit(cp)) {
            scan_number();
            return;
        }

        if (is_quote(static_cast<char>(cp))) {
            scan_string();
            return;
        }

        if (utf8::is_id_start(cp)) {
            scan_identifier();
            return;
        }

        // lone & | 等非法字符的 InvalidCharacter 兜底
        scan_operator_or_punct();
    }

    void Lexer::skip_trivia() {
        while (!is_eof()) {
            const auto [cp, len] = utf8::decode_one(src_, pos_);
            if (utf8::is_whitespace(cp)) {
                advance(len);
                continue;
            }
            // // 或 # 到行尾（换行留给下一轮按空白跳过）
            if (is_line_comment_start(cp)) {
                consume_codepoints([](const utf8::codepoint cp) { return cp != '\n'; });
                continue;
            }
            break;
        }
    }

    // 当前位是否行注释起点（// 或 #，均到行尾）。
    bool Lexer::is_line_comment_start(const utf8::codepoint cp) const noexcept {
        return (cp == '/' && peek_byte(1) == '/') || cp == '#';
    }

    // 当前位是否进制前缀（0b/0o/0x，大小写均可）：是则返回进制数（2/8/16），否则 nullopt。
    Opt<u8> Lexer::radix_prefix_base() const noexcept {
        if (peek_byte(0) != '0') {
            return std::nullopt;
        }
        switch (peek_byte(1)) {
            case 'b':
            case 'B':
                return 2;
            case 'o':
            case 'O':
                return 8;
            case 'x':
            case 'X':
                return 16;
            default:
                return std::nullopt;
        }
    }

    void Lexer::scan_number() {
        if (const auto base = radix_prefix_base()) {
            return scan_radix_int(*base);
        }
        return scan_decimal_or_float();
    }

    void Lexer::scan_radix_int(const u8 base) {
        // 惯用法：start = 入口 pos_，后续 pos_ 推进，span/lexeme/token 位置均用 start（各扫描器同此）。
        const u32 start = pos_;
        advance(2); // 消费前缀 0x/0b/0o

        // 前缀后必须紧跟一个进制数字（至少一位，首字符不能是 _）。
        if (is_eof() || !is_radix_digit(base, src_[pos_])) {
            error(ErrorCode::InvalidNumber, "expected a digit after the base prefix", start);
            return;
        }
        consume_ascii([base](const char ch) { return is_radix_digit(base, ch) || ch == '_'; });

        const auto lex        = StringView{src_.data() + start, pos_ - start};
        const auto lex_no_tag = StringView{src_.data() + start + 2, pos_ - start - 2}; // 剥掉 2 字节前缀

        if (!validate_underscores(lex_no_tag, base)) {
            error(ErrorCode::InvalidNumber, "invalid underscore placement in number literal", start);
            return;
        }

        if (const auto value = parse_int(lex_no_tag, base)) {
            tokens_.push_back(Token::make_integer(*value, lex, SourceLoc{&source_, start}));
        } else {
            error(ErrorCode::InvalidNumber, "integer literal out of range", start);
        }
    }

    void Lexer::scan_decimal_or_float() {
        // 调用方保证进入时以数字开头（整数部分至少一位）；has_dot/has_exp 决定最终是 float 还是 int。
        const u32 start   = pos_; // 入口即起点
        bool      has_dot = false;
        bool      has_exp = false;

        consume_ascii([](const char ch) { return is_digit(ch) || ch == '_'; });

        // 仅当 . 后紧跟数字才消费（禁 5. 这类不完整浮点；. 后非数字留给字段访问，如 5.foo）。
        if (peek_byte(0) == '.' && is_digit(peek_byte(1))) {
            has_dot = true;
            advance(); // 消费 .
            consume_ascii([](const char ch) { return is_digit(ch) || ch == '_'; });
        }

        // 指数：含指数一律作 float；e 后须有数字，否则回退把 e 留下。
        if (peek_byte(0) == 'e' || peek_byte(0) == 'E') {
            const u32 exp_pos = pos_; // 回退点：扫描状态只有偏移，回退即改游标
            advance();                // 消费 e/E
            if (peek_byte(0) == '+' || peek_byte(0) == '-') {
                advance();
            }
            bool exp_digit = false;
            while (!is_eof() && (is_digit(src_[pos_]) || src_[pos_] == '_')) {
                if (is_digit(src_[pos_])) {
                    exp_digit = true;
                }
                advance();
            }
            if (exp_digit) {
                has_exp = true;
            } else {
                pos_ = exp_pos;
            }
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};

        if (!validate_underscores(lex, 10)) {
            error(ErrorCode::InvalidNumber, "invalid underscore placement in number literal", start);
            return;
        }

        if (has_dot || has_exp) {
            if (const auto value = parse_float(lex)) {
                tokens_.push_back(Token::make_float(*value, lex, SourceLoc{&source_, start}));
            } else {
                error(ErrorCode::InvalidNumber, "float literal out of range", start);
            }
        } else {
            if (const auto value = parse_int(lex, 10)) {
                tokens_.push_back(Token::make_integer(*value, lex, SourceLoc{&source_, start}));
            } else {
                error(ErrorCode::InvalidNumber, "integer literal out of range", start);
            }
        }
    }

    void Lexer::scan_string() {
        const u32  start = pos_;
        const char quote = src_[pos_];
        advance(); // 消费开引号

        String value;
        while (true) {
            if (is_fatal_) {
                return;
            }
            if (is_eof()) {
                error(ErrorCode::UnterminatedString, "unterminated string", start);
                return;
            }
            const char c = src_[pos_];
            if (c == '\n') {
                // 裸换行：记错后推进到换行后，主循环从下一行继续
                error(ErrorCode::UnterminatedString, "unterminated string: line break in literal", start);
                advance(); // 跨过换行，让后续能继续扫
                return;
            }
            if (c == quote) {
                advance(); // 消费闭引号
                break;
            }
            if (c == '\\') {
                scan_escape(value);
                continue;
            }
            // 普通字符段（含多字节 UTF-8）：分隔符均为 ASCII（续接字节恒 >= 0x80），按字节消费
            // 不会停在码点中间且无需解码，整段原样追加。
            const u32 run_begin = pos_;
            const u8  delim     = static_cast<u8>(quote);
            consume_u8([delim](const u8 byte) { return byte != delim && byte != '\\' && byte != '\n'; });
            value.append(src_.data() + run_begin, pos_ - run_begin);
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};
        tokens_.push_back(Token::make_string(std::move(value), lex, SourceLoc{&source_, start}));
    }

    // 解析转义序列（pos_ 指向 '\\'）；可恢复错误记账后追加原样继续；\ 在串尾不报错，
    // 由 scan_string 的 is_eof 统一报 "unterminated string"。
    void Lexer::scan_escape(String& value) {
        advance(); // 消费 '\'
        if (is_eof()) {
            return;
        }

        const auto append_simple = [&](const char ch) {
            value.push_back(ch);
            advance();
        };

        switch (const char c = src_[pos_]) {
            case '"':
                return append_simple('"');
            case '\'':
                return append_simple('\'');
            case '\\':
                return append_simple('\\');
            case 'n':
                return append_simple('\n');
            case 't':
                return append_simple('\t');
            case 'r':
                return append_simple('\r');
            case '0':
                return append_simple('\0');
            case 'u': {
                advance(); // 消费 u
                if (peek_byte(0) != '{') {
                    error(ErrorCode::InvalidEscape, "expected '{' after '\\u'", pos_ - 1);
                    value += "\\u";
                    return;
                }
                advance(); // 消费 {

                // 收集 } 前的字符到 lex；非 hex 字符留给 from_chars 检测。
                const u32 start = pos_;
                consume_codepoints([](const utf8::codepoint cp) { return cp != '}'; });

                if (peek_byte(0) != '}') {
                    error(ErrorCode::InvalidEscape, "expected '}' to close '\\u{'", start - 1);
                    value += "\\u";
                    return;
                }
                advance(); // 消费 }

                // 直接解析 hex 为码点，不剥 _（文法 hex+ 不含 _，\u{1_2} 非法：'_' 非十六进制数字，整串消费必败）。
                const auto lex    = StringView{src_.data() + start, pos_ - start - 1};
                const auto parsed = util::try_parse<i64>(lex, 16);
                if (!parsed) {
                    error(ErrorCode::InvalidEscape, "expected hex digits in '\\u{...}'", start - 1);
                    value += "\\u";
                    return;
                }

                const u32 cp = static_cast<u32>(*parsed);
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    error(ErrorCode::InvalidEscape, "invalid codepoint in '\\u{...}'", start - 1);
                    value.push_back(static_cast<char>(utf8::kReplacementChar));
                    return;
                }
                value += utf8::encode(cp);
                return;
            }
            default:
                error(ErrorCode::InvalidEscape, std::format("invalid escape sequence '\\{}'", c), pos_ - 1);
                value.push_back('\\');
                value.push_back(c);
                advance(); // 消费该字符
                return;
        }
    }

    void Lexer::scan_identifier() {
        const u32 start = pos_;
        // 主循环已判 is_id_start（is_id_continue 对起始字符恒真），首码点由本循环一并消费
        consume_codepoints(utf8::is_id_continue);

        const auto lex = StringView{src_.data() + start, pos_ - start};

        if (lex.size() == 1 && lex[0] == '_') {
            tokens_.emplace_back(TokenType::Underscore, lex, SourceLoc{&source_, start});
            return;
        }
        if (const auto kw = lookup_keyword(lex)) {
            tokens_.emplace_back(*kw, lex, SourceLoc{&source_, start});
            return;
        }
        tokens_.emplace_back(TokenType::Identifier, lex, SourceLoc{&source_, start});
    }

    void Lexer::scan_operator_or_punct() {
        const u32 start = pos_;
        // 非 ASCII 码点记 InvalidCharacter（主循环已判定它不是空白/数字/标识符起始）
        if (!utf8::is_ascii(src_[pos_])) {
            const auto [cp, len] = utf8::decode_one(src_, pos_);
            error(ErrorCode::InvalidCharacter, std::format("invalid character U+{:04X}", cp), start);
            advance(len); // 推进一个码点确保前进
            return;
        }

        const auto make_token = [&](const TokenType t) {
            const u32 end = pos_;
            tokens_.emplace_back(t, StringView{src_.data() + start, end - start}, SourceLoc{&source_, start});
        };

        switch (static_cast<char>(src_[pos_])) {
            case '+':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::PlusEqual);
                } else if (peek_byte(1) == '+') {
                    advance(2);
                    make_token(TokenType::PlusPlus);
                } else {
                    advance();
                    make_token(TokenType::Plus);
                }
                return;
            case '-':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::MinusEqual);
                } else if (peek_byte(1) == '-') {
                    advance(2);
                    make_token(TokenType::MinusMinus);
                } else {
                    advance();
                    make_token(TokenType::Minus);
                }
                return;
            case '*':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::StarEqual);
                } else {
                    advance();
                    make_token(TokenType::Star);
                }
                return;
            case '/':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::SlashEqual);
                } else {
                    advance();
                    make_token(TokenType::Slash);
                }
                return;
            case '%':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::PercentEqual);
                } else {
                    advance();
                    make_token(TokenType::Percent);
                }
                return;
            case '=':
                if (peek_byte(1) == '=' && peek_byte(2) == '=') {
                    advance(3);
                    make_token(TokenType::EqualEqualEqual);
                } else if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::EqualEqual);
                } else if (peek_byte(1) == '>') {
                    advance(2);
                    make_token(TokenType::FatArrow);
                } else {
                    advance();
                    make_token(TokenType::Equal);
                }
                return;
            case '!':
                if (peek_byte(1) == '=' && peek_byte(2) == '=') {
                    advance(3);
                    make_token(TokenType::BangEqualEqual);
                } else if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::BangEqual);
                } else {
                    advance();
                    make_token(TokenType::Bang);
                }
                return;
            case '>':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::GreaterEqual);
                } else {
                    advance();
                    make_token(TokenType::Greater);
                }
                return;
            case '<':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::LessEqual);
                } else {
                    advance();
                    make_token(TokenType::Less);
                }
                return;
            case '&':
                if (peek_byte(1) == '&') {
                    advance(2);
                    make_token(TokenType::AndAnd);
                } else {
                    advance();
                    error(ErrorCode::InvalidCharacter, "expected '&&', got '&'", start);
                }
                return;
            case '|':
                if (peek_byte(1) == '|') {
                    advance(2);
                    make_token(TokenType::OrOr);
                } else {
                    advance();
                    error(ErrorCode::InvalidCharacter, "expected '||', got '|'", start);
                }
                return;
            case '.':
                if (peek_byte(1) == '.') {
                    if (peek_byte(2) == '.') {
                        advance(3);
                        make_token(TokenType::DotDotDot);
                    } else {
                        advance(2);
                        make_token(TokenType::DotDot);
                    }
                } else {
                    advance();
                    make_token(TokenType::Dot);
                }
                return;
            case '(':
                advance();
                make_token(TokenType::LeftParen);
                return;
            case ')':
                advance();
                make_token(TokenType::RightParen);
                return;
            case '{':
                advance();
                make_token(TokenType::LeftBrace);
                return;
            case '}':
                advance();
                make_token(TokenType::RightBrace);
                return;
            case '[':
                advance();
                make_token(TokenType::LeftBracket);
                return;
            case ']':
                advance();
                make_token(TokenType::RightBracket);
                return;
            case ',':
                advance();
                make_token(TokenType::Comma);
                return;
            case ':':
                advance();
                make_token(TokenType::Colon);
                return;
            case ';':
                advance();
                make_token(TokenType::Semicolon);
                return;
            default:
                advance();
                error(ErrorCode::InvalidCharacter,
                      std::format("invalid character U+{:04X}", utf8::decode_one(src_, start).first), start);
                return;
        }
    }

} // namespace aria
