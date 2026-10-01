#include "Lexer.hpp"
#include <charconv>
#include "util/utf8.hpp"

namespace aria {

    // 数字字面量解析辅助(匿名命名空间,仅本文件可见,无状态纯函数)
    namespace {

        bool is_digit(const char ch) { return ch >= '0' && ch <= '9'; }

        // 合法进制范围: 2 ~ 36
        bool is_radix_digit(const int base, const char ch) {
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

        // 校验数字字面量中 _ 的位置：_ 必须位于两个进制数字之间。
        // lex 不含进制前缀（调用方已跳过 0x/0b/0o）。base 决定何为合法数字
        // （hex 的 a-f/A-F 也算，故用 is_radix_digit 而非 is_digit）。
        // 非法位置：首尾的 _、紧邻 . /e/E/+/- 的 _。
        bool validate_underscores(const StringView lex, const int base) {
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

        // 剥去 lex 中的 _，写入 out（null 结尾）。超长（>= out_cap）返回 false。
        bool strip_underscores(const StringView lex, char* out, const usize out_cap) {
            usize out_size = 0;
            for (const char i: lex) {
                if (i == '_') {
                    continue;
                }
                if (out_size + 1 >= out_cap) { // 留一个给 '\0'
                    return false;
                }

                out[out_size++] = i;
            }
            out[out_size] = '\0';
            return true;
        }

        // 解析整数字面量为 i64（lex 不含进制前缀、不含 e/E 指数--含指数的走 float）。
        // base 为 2/8/10/16。纯整数，from_chars 解析；溢出由其返回 result_out_of_range。
        bool parse_int(const StringView lex, const int base, i64& out) {
            char buf[64];
            if (!strip_underscores(lex, buf, sizeof(buf))) {
                return false;
            }
            const usize len      = std::strlen(buf);
            const auto [ptr, ec] = std::from_chars(buf, buf + len, out, base);
            return ec == std::errc{} && ptr == buf + len;
        }

        // 解析浮点字面量为 f64。含小数点或 e/E 指数（科学计数法）的数字字面量统一走此。
        bool parse_float(const StringView lex, f64& out) {
            char buf[64];
            if (!strip_underscores(lex, buf, sizeof(buf))) {
                return false;
            }
            const usize len      = std::strlen(buf);
            const auto [ptr, ec] = std::from_chars(buf, buf + len, out);
            return ec == std::errc{} && ptr == buf + len;
        }
    } // namespace

    // 一次性实例：构造即注入扫描状态（契约见 Lexer.hpp 构造注）。
    Lexer::Lexer(SourceFile& src) noexcept :
        source_{src}, src_{src.content()}, pos_{0}, tokens_{}, errors_{}, is_fatal_{false} {}

    Result<List<Token>, List<Error>> Lexer::tokenize(SourceFile& src) {
        Lexer lexer{src};
        lexer.run();

        // 存在任何错误（含达上限 is_fatal_，此时 errors_ 必非空）-> 返回错误集合；否则 token 流。
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
            const auto cp = utf8::decode_one(src_, pos_).first; // 各分支自行解码推进

            // 空白与注释（// 或 #）：统一交给 skip_trivia 跳过
            if (utf8::is_whitespace(cp) || (cp == '/' && peek_byte(1) == '/') || cp == '#') {
                skip_trivia();
                continue;
            }

            // 数字：以数字开头（. 不启动数字扫描--禁止 .5 这类不完整浮点，. 留给 Dot token）
            if (utf8::is_digit(cp)) {
                scan_number();
                continue;
            }

            if (cp == '"' || cp == '\'') {
                scan_string();
                continue;
            }

            if (utf8::is_id_start(cp)) {
                scan_identifier();
                continue;
            }

            // 含 lone & | 等非法字符的 InvalidCharacter 兜底
            scan_operator_or_punct();
        }

        if (is_fatal_) {
            return; // 致命错误，不补 Eof
        }
        // EOF token 位置 = 内容末尾偏移；其行列由 SourceLoc 派生为「下一行第 1 列」
        // （编辑器光标停文件末尾的约定，见 source_file.hpp line_at）。
        const u32 eof = src_.size();
        tokens_.push_back(Token{TokenType::Eof, {}, SourceLoc{&source_, eof}});
    }

    void Lexer::skip_trivia() {
        while (!is_eof()) {
            const auto [cp, len] = utf8::decode_one(src_, pos_);
            if (utf8::is_whitespace(cp)) {
                advance(len);
                continue;
            }
            // // 或 # 到行尾（换行留给下一轮按空白跳过）
            if ((cp == '/' && peek_byte(1) == '/') || cp == '#') {
                consume_codepoints([](const utf8::codepoint cp) { return cp != '\n'; });
                continue;
            }
            break;
        }
    }

    void Lexer::scan_number() {
        // 判定进制前缀：0b/0o/0x
        if (peek_byte(0) == '0') {
            const char peeked = peek_byte(1);
            if (peeked == 'b' || peeked == 'B' || peeked == 'o' || peeked == 'O' || peeked == 'x' || peeked == 'X') {
                return scan_radix_int();
            }
        }
        return scan_decimal_or_float();
    }

    void Lexer::scan_radix_int() {
        // 惯用法：start = 入口即起点，后续 pos_ 推进，span/lexeme/token 位置用 start（各扫描器同此）。
        const u32  start = pos_;
        const char tag   = peek_byte(1); // b/B/o/O/x/X
        const int  base  = (tag == 'b' || tag == 'B') ? 2 : (tag == 'o' || tag == 'O') ? 8 : 16;
        advance(2); // 消费前缀 0x/0b/0o

        // 前缀后必须紧跟一个进制数字（文法：0x[0-9a-fA-F]... 至少一位，首字符不能是 _）。
        // 直接断言，避免循环消费后再判，报错更早更准。
        if (is_eof() || !is_radix_digit(base, src_[pos_])) {
            error(ErrorCode::InvalidNumber, "expected a digit after the base prefix", start);
            return;
        }
        // 消费后续的数字与 _（_ 位置合法性由 validate_underscores 校验）
        consume_ascii([base](const char ch) { return is_radix_digit(base, ch) || ch == '_'; });

        const auto lex        = StringView{src_.data() + start, pos_ - start};
        const auto lex_no_tag = StringView{src_.data() + start + 2, pos_ - start - 2}; // 剥掉 2 字节前缀

        if (!validate_underscores(lex_no_tag, base)) {
            error(ErrorCode::InvalidNumber, "invalid underscore placement in number literal", start);
            return;
        }

        i64 value = 0;
        if (!parse_int(lex_no_tag, base, value)) {
            error(ErrorCode::InvalidNumber, "integer literal out of range", start);
            return;
        }
        tokens_.push_back(Token::make_integer(value, lex, SourceLoc{&source_, start}));
    }

    void Lexer::scan_decimal_or_float() {
        // 按 整数 -> 小数 -> 指数 顺序线性扫描。has_dot/has_exp 决定最终是 float 还是 int。
        // 调用方（主循环）保证进入时以数字开头，故整数部分至少一位，无需 has_digit 校验。
        const u32 start   = pos_; // 入口即起点（惯用法见 scan_radix_int 注）
        bool      has_dot = false;
        bool      has_exp = false;

        // 整数部分：消费连续的数字与 _（入口必是数字，故非空；_ 位置合法性由 validate_underscores 校验）
        consume_ascii([](const char ch) { return is_digit(ch) || ch == '_'; });

        // 小数部分：仅当 . 后紧跟数字时才消费（禁止 5. / 1. 这类点后无数字的不完整浮点；
        // . 后非数字则不消费，把 . 留给 operator/punct，如 5.foo 走字段访问）。
        if (peek_byte(0) == '.' && is_digit(peek_byte(1))) {
            has_dot = true;
            advance(); // 消费 .
            consume_ascii([](const char ch) { return is_digit(ch) || ch == '_'; });
        }

        // 指数部分：当前是 e/E。含指数一律作 float。e 后须有数字，否则回退把 e 留下。
        if (peek_byte(0) == 'e' || peek_byte(0) == 'E') {
            const u32 exp_pos = pos_; // 回退点：位置状态只有偏移，回退即改游标，无需还原其他状态
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
            f64 value = 0.0;
            if (!parse_float(lex, value)) {
                error(ErrorCode::InvalidNumber, "float literal out of range", start);
                return;
            }
            tokens_.push_back(Token::make_float(value, lex, SourceLoc{&source_, start}));
        } else {
            i64 value = 0;
            if (!parse_int(lex, 10, value)) {
                error(ErrorCode::InvalidNumber, "integer literal out of range", start);
                return;
            }
            tokens_.push_back(Token::make_integer(value, lex, SourceLoc{&source_, start}));
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
                // 串未闭合：记错后推进到 EOF，主循环继续扫后续（若还有内容）
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
            // 普通字符段（含多字节 UTF-8）：消费到分隔符（引号 / 反斜杠 / 换行）或 EOF，整段原样追加。
            // 分隔符都是 ASCII、UTF-8 续接字节恒 >= 0x80，故按字节消费不会停在码点中间，也不需解码
            // （只需跳过）--用 consume_u8 而非 consume_codepoints：后者每个多字节码点要解码一次。
            const u32 run_begin = pos_;
            const u8  delim     = static_cast<u8>(quote);
            consume_u8([delim](const u8 byte) { return byte != delim && byte != '\\' && byte != '\n'; });
            value.append(src_.data() + run_begin, pos_ - run_begin);
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};
        tokens_.push_back(Token::make_string(std::move(value), lex, SourceLoc{&source_, start}));
    }

    // 解析转义序列（pos_ 指向 '\\'）。
    // 成功追加到 value；可恢复错误（InvalidEscape）记账后追加原样继续；
    // 遇 EOF（\ 在串尾）不报错，由 scan_string 的 is_eof 统一报 "unterminated string"。
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

                // 收集 } 前的字符到 lex（与数字扫描一致：遇终止符停）。
                // 非 hex 字符（如 \u{12g}）留给 from_chars 检测。
                const u32 start = pos_;
                consume_codepoints([](const utf8::codepoint cp) { return cp != '}'; });

                if (peek_byte(0) != '}') {
                    // \u{...} 到 EOF 也无 }
                    error(ErrorCode::InvalidEscape, "expected '}' to close '\\u{'", start - 1);
                    value += "\\u";
                    return;
                }
                advance(); // 消费 }

                // 用 from_chars 直接解析 hex 内容为码点（不剥 _：文法 hex+ 不含 _，
                // 故 \u{1_2} 非法；from_chars 遇非 hex 字符或 _ 会停下，ptr != last 即报错）。
                const auto lex    = StringView{src_.data() + start, pos_ - start - 1};
                i64        cp_i64 = 0;
                if (const auto [ptr, ec] = std::from_chars(lex.data(), lex.data() + lex.size(), cp_i64, 16);
                    ec != std::errc{} || ptr != lex.data() + lex.size()) {
                    error(ErrorCode::InvalidEscape, "expected hex digits in '\\u{...}'", start - 1);
                    value += "\\u";
                    return;
                }

                const u32 cp = static_cast<u32>(cp_i64);
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
        // 主循环已判 is_id_start，而 is_id_continue 对起始字符恒真，故首码点由本循环一并消费
        consume_codepoints(utf8::is_id_continue);

        const auto lex = StringView{src_.data() + start, pos_ - start};

        // 单独 _ -> Underscore
        if (lex.size() == 1 && lex[0] == '_') {
            tokens_.emplace_back(TokenType::Underscore, lex, SourceLoc{&source_, start});
            return;
        }
        // 关键字优先于 identifier
        if (const auto kw = lookup_keyword(lex)) {
            tokens_.emplace_back(*kw, lex, SourceLoc{&source_, start});
            return;
        }
        tokens_.emplace_back(TokenType::Identifier, lex, SourceLoc{&source_, start});
    }

    void Lexer::scan_operator_or_punct() {
        const u32 start = pos_;
        // 非 ASCII 码点 -> InvalidCharacter（主循环已判定它不是空白/数字/标识符起始）
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
