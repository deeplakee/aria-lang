#include "Lexer.hpp"
#include "util/utf8.hpp"
#include "util/util.hpp"

namespace aria {

    namespace {

        bool is_digit(const char ch) { return ch >= '0' && ch <= '9'; }

        // 引号字符（串字面量的两种定界形态 " 与 '）
        bool is_quote(const utf8::codepoint cp) { return cp == '"' || cp == '\''; }

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
                return util::try_parse<i64>({buf, *len}, base);
            }
            return std::nullopt;
        }

        Opt<f64> parse_float(const StringView lex) {
            char buf[64];
            if (const auto len = strip_underscores(lex, buf, sizeof(buf))) {
                return util::try_parse<f64>({buf, *len});
            }
            return std::nullopt;
        }
    } // namespace

    Lexer::Lexer(SourceFile& src) noexcept : source_{src}, src_{src.content()}, pos_{0}, start_{0}, tokens_{} {}

    Result<List<Token>, Error> Lexer::tokenize(SourceFile& src) {
        Lexer lexer{src};
        try {
            lexer.run();
        } catch (const AriaCompileException& e) {
            return std::unexpected(e.error());
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

    void Lexer::run() {
        while (!is_eof()) {
            dispatch_one();
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

        start_ = pos_; // token 起点 = 错误锚点（span/lexeme/token 位置均用此，各扫描器同）

        // 以数字开头才走数字扫描（. 不启动数字--禁 .5 这类不完整浮点，. 留给 Dot token）。
        if (utf8::is_digit(cp)) {
            scan_number();
        } else if (is_quote(cp)) {
            scan_string(static_cast<char>(cp));
        } else if (utf8::is_id_start(cp)) {
            scan_identifier();
        } else {
            // lone & | 等非法字符的 InvalidCharacter 兜底
            scan_operator_or_punct();
        }
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
        advance(2); // 消费前缀 0x/0b/0o

        // 前缀后必须紧跟一个进制数字（至少一位，首字符不能是 _）。
        if (is_eof() || !is_radix_digit(base, src_[pos_])) {
            error(ErrorCode::InvalidNumber, "expected a digit after the base prefix");
        }
        consume_ascii([base](const char ch) { return is_radix_digit(base, ch) || ch == '_'; });

        const auto lex        = StringView{src_.data() + start_, pos_ - start_};
        const auto lex_no_tag = StringView{src_.data() + start_ + 2, pos_ - start_ - 2}; // 剥掉 2 字节前缀

        if (!validate_underscores(lex_no_tag, base)) {
            error(ErrorCode::InvalidNumber, "invalid underscore placement in number literal");
        }

        if (const auto value = parse_int(lex_no_tag, base)) {
            tokens_.push_back(Token::make_integer(*value, lex, SourceLoc{&source_, start_}));
        } else {
            error(ErrorCode::InvalidNumber, "integer literal out of range");
        }
    }

    void Lexer::scan_decimal_or_float() {
        // 调用方保证进入时以数字开头（整数部分至少一位）；has_dot/has_exp 决定最终是 float 还是 int。
        bool has_dot = false;
        bool has_exp = false;

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

        const auto lex = StringView{src_.data() + start_, pos_ - start_};

        if (!validate_underscores(lex, 10)) {
            error(ErrorCode::InvalidNumber, "invalid underscore placement in number literal");
        }

        if (has_dot || has_exp) {
            if (const auto value = parse_float(lex)) {
                tokens_.push_back(Token::make_float(*value, lex, SourceLoc{&source_, start_}));
            } else {
                error(ErrorCode::InvalidNumber, "float literal out of range");
            }
        } else {
            if (const auto value = parse_int(lex, 10)) {
                tokens_.push_back(Token::make_integer(*value, lex, SourceLoc{&source_, start_}));
            } else {
                error(ErrorCode::InvalidNumber, "integer literal out of range");
            }
        }
    }

    void Lexer::scan_string(const char quote) {
        const u32 start = pos_; // 串 token 跨度构造用（start_ 会被 scan_escape 重指，勿依赖）
        advance();              // 消费开引号

        String value;
        while (true) {
            if (is_eof()) {
                error(ErrorCode::UnterminatedString, "unterminated string");
            }
            const char c = src_[pos_];
            if (c == '\n') {
                error(ErrorCode::UnterminatedString, "unterminated string: line break in literal");
            }
            if (c == quote) {
                advance(); // 消费闭引号
                break;
            }
            if (c == '\\') {
                scan_escape(value);
                start_ = start; // scan_escape 重指过锚点，恢复为串起点
                continue;
            }
            // 普通字符段（含多字节 UTF-8）：分隔符均为 ASCII（续接字节恒 >= 0x80），按字节消费
            // 不会停在码点中间且无需解码，整段原样追加。
            const u32 run_begin = pos_;
            consume_byte([quote](const char byte) { return byte != quote && byte != '\\' && byte != '\n'; });
            value.append(src_.data() + run_begin, pos_ - run_begin);
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};
        tokens_.push_back(Token::make_string(std::move(value), lex, SourceLoc{&source_, start}));
    }

    // 解析转义序列（pos_ 指向 '\\'）；错误即抛出（首错即止），锚点入口重指到 '\'；\ 在串尾不报错，
    // 由 scan_string 的 is_eof 统一报 "unterminated string"。
    void Lexer::scan_escape(String& value) {
        start_ = pos_; // 锚点重指到转义起点
        advance();     // 消费 '\'
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
                    error(ErrorCode::InvalidEscape, "expected '{{' after '\\u'");
                }
                advance(); // 消费 {

                // 收集 } 前的字符到 lex；非 hex 字符留给 from_chars 检测。
                const u32 start = pos_;
                consume_codepoints([](const utf8::codepoint cp) { return cp != '}'; });

                if (peek_byte(0) != '}') {
                    error(ErrorCode::InvalidEscape, "expected '}}' to close '\\u{{'");
                }
                advance(); // 消费 }

                // 直接解析 hex 为码点，不剥 _（文法 hex+ 不含 _，\u{1_2} 非法：'_' 非十六进制数字，整串消费必败）。
                const auto lex    = StringView{src_.data() + start, pos_ - start - 1};
                const auto parsed = util::try_parse<i64>(lex, 16);
                if (!parsed) {
                    error(ErrorCode::InvalidEscape, "expected hex digits in '\\u{{...}}'");
                }

                const u32 cp = static_cast<u32>(*parsed);
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    error(ErrorCode::InvalidEscape, "invalid codepoint in '\\u{{...}}'");
                }
                value += utf8::encode(cp);
                return;
            }
            default:
                error(ErrorCode::InvalidEscape, "invalid escape sequence '\\{}'", c);
        }
    }

    void Lexer::scan_identifier() {
        // 主循环已判 is_id_start（is_id_continue 对起始字符恒真），首码点由本循环一并消费
        consume_codepoints(utf8::is_id_continue);

        const auto lex = StringView{src_.data() + start_, pos_ - start_};

        if (lex.size() == 1 && lex[0] == '_') {
            tokens_.emplace_back(TokenType::Underscore, lex, SourceLoc{&source_, start_});
            return;
        }
        if (const auto kw = lookup_keyword(lex)) {
            tokens_.emplace_back(*kw, lex, SourceLoc{&source_, start_});
            return;
        }
        tokens_.emplace_back(TokenType::Identifier, lex, SourceLoc{&source_, start_});
    }

    void Lexer::scan_operator_or_punct() {
        // 非 ASCII 码点记 InvalidCharacter（主循环已判定它不是空白/数字/标识符起始）
        if (!utf8::is_ascii(src_[pos_])) {
            const auto cp = utf8::decode_one(src_, pos_).first;
            error(ErrorCode::InvalidCharacter, "invalid character U+{:04X}", cp);
        }

        const auto make_token = [&](const TokenType t) {
            const u32 end = pos_;
            tokens_.emplace_back(t, StringView{src_.data() + start_, end - start_}, SourceLoc{&source_, start_});
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
                    error(ErrorCode::InvalidCharacter, "expected '&&', got '&'");
                }
                return;
            case '|':
                if (peek_byte(1) == '|') {
                    advance(2);
                    make_token(TokenType::OrOr);
                } else {
                    error(ErrorCode::InvalidCharacter, "expected '||', got '|'");
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
                error(ErrorCode::InvalidCharacter, "invalid character U+{:04X}", utf8::decode_one(src_, start_).first);
        }
    }

} // namespace aria
