#include "Lexer.hpp"
#include <charconv>
#include "util/utf8.hpp"

// 生成按引用捕获的 char 谓词 lambda，用于 conditional_advance 等接受 char 谓词的场合。
// 仅因「lambda 样板噪音掩盖判断逻辑」这一 C++ 语言层面难以消除的痛点而引入；
// 非此场景勿滥用宏。用法：conditional_advance(ARIA_CHAR_PRED_REF(c != '\n'));
#define ARIA_CHAR_PRED_REF(cond) [&](const char c) { return (cond); }

namespace aria {

    // ============================================================
    // 数字字面量解析辅助（匿名命名空间,仅本文件可见;无状态纯函数）
    // ============================================================
    namespace {

        // 判定 c 是否为十进制数字字符。
        bool is_digit(const char c) { return c >= '0' && c <= '9'; }

        // 判定 c 是否为 base 进制的合法数字字符。
        // 合法进制范围: 2 ~ 36
        bool is_radix_digit(const int base, const char c) {
            if (base < 2 || base > 36) {
                return false;
            }
            if (base <= 10) {
                return c >= '0' && c < static_cast<char>('0' + base);
            }

            const bool lower = (c >= 'a' && c < static_cast<char>('a' + base - 10));
            const bool upper = (c >= 'A' && c < static_cast<char>('A' + base - 10));
            return is_digit(c) || lower || upper;
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

    // ============================================================
    // 构造与入口
    // ============================================================

    Lexer::Lexer() noexcept : source_{nullptr}, src_{}, pos_{0}, tokens_{}, errors_{}, is_fatal_{false} {}

    Result<List<Token>, List<Error>> Lexer::tokenize(SourceFile* src) {
        // 注入扫描状态
        source_   = src;
        src_      = src->content();
        pos_      = 0;
        is_fatal_ = false;

        run();

        // 收尾：存在任何错误（含达上限 is_fatal_，此时 errors_ 必非空）-> 返回错误集合；
        // 否则返回 token 流。
        List<Token> out_tokens = std::move(tokens_);
        List<Error> out_errors = std::move(errors_);
        const bool  had_error  = !out_errors.empty();

        // 清空成员，回到空态，供 Lexer 复用
        source_   = nullptr;
        src_      = {};
        pos_      = 0;
        is_fatal_ = false;
        tokens_.clear();
        errors_.clear();

        if (had_error) {
            return std::unexpected(std::move(out_errors));
        }
        return out_tokens;
    }

    // ============================================================
    // 游标辅助
    // ============================================================

    char Lexer::peek_byte(const usize ahead) const noexcept {
        const usize i = pos_ + ahead;
        // src_ 底层以 '\0' 结尾且 content() 保证 size 以内有数据；
        // 越过真实内容返回 '\0'（哨兵），安全。
        if (i >= src_.size()) {
            return '\0';
        }
        return src_[i];
    }

    utf8::codepoint Lexer::peek_codepoint(const usize ahead) const noexcept {
        const usize i = pos_ + ahead;
        if (i >= src_.size()) {
            return 0;
        }
        return utf8::decode_one(src_, i).first;
    }

    void Lexer::advance(const usize n) noexcept {
        ASSERT(pos_ + n <= src_.size(), "Lexer::advance 推进越界");
        pos_ += n;
    }

    bool Lexer::is_eof() const noexcept { return pos_ >= src_.size(); }

    SourceLoc Lexer::loc_at(const usize offset) const { return SourceLoc{source_, source_->locate(offset)}; }

    // ============================================================
    // 错误记账
    // ============================================================

    Error Lexer::make_error(ErrorCode code, SourceSpan span, String msg) const {
        return Error{code, loc_at(span.start), std::move(msg)};
    }

    void Lexer::error(ErrorCode code, SourceSpan span, String msg) {
        errors_.push_back(make_error(code, span, std::move(msg)));
        // 达上限即停止扫描，避免级联错误刷屏
        if (errors_.size() >= kMaxErrors) {
            is_fatal_ = true;
        }
    }

    // ============================================================
    // 主循环
    // ============================================================

    void Lexer::run() {
        while (!is_fatal_ && !is_eof()) {
            const auto cp = utf8::decode_one(src_, pos_).first; // 各分支自行解码推进

            // 空白与注释（// 或 #）：统一交给 skip_trivia 跳过
            if (utf8::is_whitespace(cp) || (cp == '/' && peek_byte(1) == '/') || cp == '#') {
                skip_trivia();
                continue;
            }

            // 数字：以数字开头（. 不再启动数字扫描--禁止 .5 这类不完整浮点，. 留给 Dot token）
            if (utf8::is_digit(cp)) {
                scan_number();
                continue;
            }

            // 字符串
            if (cp == '"' || cp == '\'') {
                scan_string();
                continue;
            }

            // 标识符 / 关键字 / _
            if (utf8::is_id_start(cp)) {
                scan_identifier();
                continue;
            }

            // 运算符 / 标点（含 lone & | 等非法字符的 InvalidCharacter 兜底）
            scan_operator_or_punct(cp);
        }

        if (is_fatal_) {
            return; // 致命错误，不补 Eof
        }
        // 末尾 Eof token
        const usize eof = src_.size();
        tokens_.push_back(Token{TokenType::Eof, {}, loc_at(eof)});
    }

    void Lexer::skip_trivia() {
        while (!is_eof()) {
            const auto [cp, len] = utf8::decode_one(src_, pos_);
            if (utf8::is_whitespace(cp)) {
                advance(len);
                continue;
            }
            // // 或 # 到行尾
            if ((cp == '/' && peek_byte(1) == '/') || cp == '#') {
                conditional_advance(ARIA_CHAR_PRED_REF(c != '\n'));
                continue;
            }
            break;
        }
    }

    // ============================================================
    // 数字
    // ============================================================

    void Lexer::scan_number() {
        // 判定进制前缀：0b/0o/0x
        if (peek_byte(0) == '0') {
            const char p = peek_byte(1);
            if (p == 'b' || p == 'B' || p == 'o' || p == 'O' || p == 'x' || p == 'X') {
                return scan_radix_int();
            }
        }
        return scan_decimal_or_float();
    }

    void Lexer::scan_radix_int() {
        const usize start = pos_;         // 入口即起点；后续 pos_ 推进，span/lexeme 用 start
        const char  tag   = peek_byte(1); // b/B/o/O/x/X
        const int   base  = (tag == 'b' || tag == 'B') ? 2 : (tag == 'o' || tag == 'O') ? 8 : 16;
        pos_              = start + 2; // 消费前缀 0x/0b/0o

        // 前缀后必须紧跟一个进制数字（文法：0x[0-9a-fA-F]... 至少一位，首字符不能是 _）。
        // 直接断言，避免循环消费后再判，报错更早更准。
        if (is_eof() || !is_radix_digit(base, src_[pos_])) {
            error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"进制字面量缺少数字"});
            return;
        }
        // 消费后续的数字与 _（_ 位置合法性由 validate_underscores 校验）
        conditional_advance(ARIA_CHAR_PRED_REF(is_radix_digit(base, c) || c == '_'));

        const auto lex = StringView{src_.data() + start, pos_ - start};
        // 去掉进制前缀 0x/0b/0o（2 字节），传给解析辅助做校验/解析
        const auto lex_no_tag = StringView{src_.data() + start + 2, pos_ - start - 2};

        if (!validate_underscores(lex_no_tag, base)) {
            error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"数字字面量下划线位置非法"});
            return;
        }

        i64 value = 0;
        if (!parse_int(lex_no_tag, base, value)) {
            error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"数字字面量解析失败"});
            return;
        }
        tokens_.push_back(Token::make_integer(value, lex, loc_at(start)));
    }

    void Lexer::scan_decimal_or_float() {
        // 按 整数 -> 小数 -> 指数 顺序线性扫描。has_dot/has_exp 决定最终是 float 还是 int。
        // 调用方（主循环）保证进入时以数字开头，故整数部分至少一位，无需 has_digit 校验。
        const usize start   = pos_; // 入口即起点；后续 pos_ 推进，span/lexeme 用 start
        bool        has_dot = false;
        bool        has_exp = false;

        // 整数部分：消费连续的数字与 _（入口必是数字，故非空；_ 位置合法性由 validate_underscores 校验）
        conditional_advance(ARIA_CHAR_PRED_REF(is_digit(c) || c == '_'));

        // 小数部分：仅当 . 后紧跟数字时才消费（禁止 5. / 1. 这类点后无数字的不完整浮点；
        // . 后非数字则不消费，把 . 留给 operator/punct，如 5.foo 走字段访问）。
        if (peek_byte(0) == '.' && is_digit(peek_byte(1))) {
            has_dot = true;
            ++pos_; // 消费 .
            conditional_advance(ARIA_CHAR_PRED_REF(is_digit(c) || c == '_'));
        }

        // 指数部分：当前是 e/E。含指数一律作 float。e 后须有数字，否则回退把 e 留下。
        if (peek_byte(0) == 'e' || peek_byte(0) == 'E') {
            const usize exp_pos = pos_;
            ++pos_; // 消费 e/E
            if (peek_byte(0) == '+' || peek_byte(0) == '-') {
                ++pos_;
            }
            bool exp_digit = false;
            while (!is_eof() && (is_digit(src_[pos_]) || src_[pos_] == '_')) {
                if (is_digit(src_[pos_])) {
                    exp_digit = true;
                }
                ++pos_;
            }
            if (exp_digit) {
                has_exp = true;
            } else {
                pos_ = exp_pos; // e 后无数字，回退把 e 留给下个 token
            }
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};

        if (!validate_underscores(lex, 10)) {
            error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"数字字面量下划线位置非法"});
            return;
        }

        // 含小数点或指数 -> float；否则 int
        if (has_dot || has_exp) {
            f64 value = 0.0;
            if (!parse_float(lex, value)) {
                error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"浮点字面量解析失败"});
                return;
            }
            tokens_.push_back(Token::make_float(value, lex, loc_at(start)));
        } else {
            i64 value = 0;
            if (!parse_int(lex, 10, value)) {
                error(ErrorCode::InvalidNumber, SourceSpan{start, pos_}, String{"数字字面量解析失败"});
                return;
            }
            tokens_.push_back(Token::make_integer(value, lex, loc_at(start)));
        }
    }

    // ============================================================
    // 字符串（plainString）
    // ============================================================

    void Lexer::scan_string() {
        const usize start = pos_;
        const char  quote = src_[pos_];
        ++pos_; // 消费开引号

        String value;
        while (true) {
            if (is_fatal_) {
                return;
            }
            if (is_eof()) {
                // 串未闭合：记错后推进到 EOF，主循环继续扫后续（若还有内容）
                error(ErrorCode::UnterminatedString, SourceSpan{start, pos_}, String{"字符串未闭合"});
                return;
            }
            const char c = src_[pos_];
            if (c == '\n') {
                // 裸换行：记错后推进到换行后，主循环从下一行继续
                error(ErrorCode::UnterminatedString, SourceSpan{start, pos_}, String{"字符串跨行未闭合"});
                ++pos_; // 跨过换行，让后续能继续扫
                return;
            }
            if (c == quote) {
                ++pos_; // 消费闭引号
                break;
            }
            if (c == '\\') {
                scan_escape(value);
                continue;
            }
            // 普通字符（含多字节 UTF-8）：原样追加
            const auto [_, len] = utf8::decode_one(src_, pos_);
            for (usize i = 0; i < len; ++i) {
                value.push_back(src_[pos_ + i]);
            }
            advance(len);
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};
        tokens_.push_back(Token::make_string(std::move(value), lex, loc_at(start)));
    }

    // 解析转义序列（pos_ 指向 '\\'）。
    // 成功追加到 value；可恢复错误（InvalidEscape）记账后追加原样继续；
    // 遇 EOF（\ 在串尾）不报错，由 scan_string 的 is_eof 统一报"字符串未闭合"。
    void Lexer::scan_escape(String& value) {
        ++pos_; // 消费 '\'
        if (is_eof()) {
            return; // 串未闭合，交给 scan_string 报错
        }

        const auto append_simple = [&](const char ch) {
            value.push_back(ch);
            ++pos_;
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
                ++pos_; // 消费 u
                if (peek_byte(0) != '{') {
                    error(ErrorCode::InvalidEscape, SourceSpan{pos_ - 1, pos_}, String{"\\u 转义缺少 '{'"});
                    value += "\\u";
                    return;
                }
                ++pos_; // 消费 {

                // 收集 } 前的字符到 lex（与数字扫描一致：遇终止符停）。
                // 非 hex 字符（如 \u{12g}）留给 from_chars 检测。
                const usize start = pos_;
                conditional_advance(ARIA_CHAR_PRED_REF(c != '}'));

                if (peek_byte(0) != '}') {
                    // \u{...} 到 EOF 也无 }
                    error(ErrorCode::InvalidEscape, SourceSpan{start - 1, pos_}, String{"\\u{...} 转义缺少 '}'"});
                    value += "\\u";
                    return;
                }
                ++pos_; // 消费 }

                // 用 from_chars 直接解析 hex 内容为码点（不剥 _：文法 hex+ 不含 _，
                // 故 \u{1_2} 非法；from_chars 遇非 hex 字符或 _ 会停下，ptr != last 即报错）。
                const auto lex    = StringView{src_.data() + start, pos_ - start - 1};
                i64        cp_i64 = 0;
                if (const auto [ptr, ec] = std::from_chars(lex.data(), lex.data() + lex.size(), cp_i64, 16);
                    ec != std::errc{} || ptr != lex.data() + lex.size()) {
                    error(ErrorCode::InvalidEscape, SourceSpan{start - 1, pos_},
                          String{"\\u{...} 转义缺少十六进制或含非合法 hex 字符"});
                    value += "\\u";
                    return;
                }

                const u32 cp = static_cast<u32>(cp_i64);
                if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                    error(ErrorCode::InvalidEscape, SourceSpan{start - 1, pos_}, String{"\\u{...} 转义码点非法"});
                    value.push_back(static_cast<char>(utf8::kReplacementChar));
                    return;
                }
                value += utf8::encode(cp);
                return;
            }
            default:
                error(ErrorCode::InvalidEscape, SourceSpan{pos_ - 1, pos_ + 1}, String{"未识别的转义序列"});
                value.push_back('\\');
                value.push_back(c);
                ++pos_;
                return;
        }
    }

    // ============================================================
    // 标识符 / 关键字
    // ============================================================

    void Lexer::scan_identifier() {
        const usize start = pos_;
        advance(utf8::decode_one(src_, start).second); // 消费首码点（主循环已判 is_id_start）
        while (!is_eof()) {
            const auto [cp, len] = utf8::decode_one(src_, pos_);
            if (utf8::is_id_continue(cp)) {
                advance(len);
            } else {
                break;
            }
        }

        const auto lex = StringView{src_.data() + start, pos_ - start};

        // 单独 _ -> Underscore
        if (lex.size() == 1 && lex[0] == '_') {
            tokens_.emplace_back(TokenType::Underscore, lex, loc_at(start));
            return;
        }
        // 关键字优先于 identifier
        if (const auto kw = lookup_keyword(lex)) {
            tokens_.emplace_back(*kw, lex, loc_at(start));
            return;
        }
        tokens_.emplace_back(TokenType::Identifier, lex, loc_at(start));
    }

    // ============================================================
    // 运算符 / 标点（最长匹配）
    // ============================================================

    void Lexer::scan_operator_or_punct(const utf8::codepoint cp) {
        const usize start = pos_;

        // 多字节非法字符 -> InvalidCharacter
        if (cp >= 0x80) {
            const usize len = utf8::decode_one(src_, pos_).second;
            error(ErrorCode::InvalidCharacter, SourceSpan{start, start + len}, String{"非法字符"});
            advance(len); // 推进一个码点确保前进
            return;
        }

        const auto make_token = [&](const TokenType t) {
            const usize end = pos_;
            tokens_.emplace_back(t, StringView{src_.data() + start, end - start}, loc_at(start));
        };

        switch (static_cast<char>(cp)) {
            case '+':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::PlusEqual);
                } else if (peek_byte(1) == '+') {
                    advance(2);
                    make_token(TokenType::PlusPlus);
                } else {
                    ++pos_;
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
                    ++pos_;
                    make_token(TokenType::Minus);
                }
                return;
            case '*':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::StarEqual);
                } else {
                    ++pos_;
                    make_token(TokenType::Star);
                }
                return;
            case '/':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::SlashEqual);
                } else {
                    ++pos_;
                    make_token(TokenType::Slash);
                }
                return;
            case '%':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::PercentEqual);
                } else {
                    ++pos_;
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
                    ++pos_;
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
                    ++pos_;
                    make_token(TokenType::Bang);
                }
                return;
            case '>':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::GreaterEqual);
                } else {
                    ++pos_;
                    make_token(TokenType::Greater);
                }
                return;
            case '<':
                if (peek_byte(1) == '=') {
                    advance(2);
                    make_token(TokenType::LessEqual);
                } else {
                    ++pos_;
                    make_token(TokenType::Less);
                }
                return;
            case '&':
                if (peek_byte(1) == '&') {
                    advance(2);
                    make_token(TokenType::AndAnd);
                } else {
                    ++pos_;
                    error(ErrorCode::InvalidCharacter, SourceSpan{start, pos_}, String{"单独的 '&' 非法"});
                }
                return;
            case '|':
                if (peek_byte(1) == '|') {
                    advance(2);
                    make_token(TokenType::OrOr);
                } else {
                    ++pos_;
                    error(ErrorCode::InvalidCharacter, SourceSpan{start, pos_}, String{"单独的 '|' 非法"});
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
                    ++pos_;
                    make_token(TokenType::Dot);
                }
                return;
            case '(':
                ++pos_;
                make_token(TokenType::LeftParen);
                return;
            case ')':
                ++pos_;
                make_token(TokenType::RightParen);
                return;
            case '{':
                ++pos_;
                make_token(TokenType::LeftBrace);
                return;
            case '}':
                ++pos_;
                make_token(TokenType::RightBrace);
                return;
            case '[':
                ++pos_;
                make_token(TokenType::LeftBracket);
                return;
            case ']':
                ++pos_;
                make_token(TokenType::RightBracket);
                return;
            case ',':
                ++pos_;
                make_token(TokenType::Comma);
                return;
            case ':':
                ++pos_;
                make_token(TokenType::Colon);
                return;
            case ';':
                ++pos_;
                make_token(TokenType::Semicolon);
                return;
            default:
                ++pos_;
                error(ErrorCode::InvalidCharacter, SourceSpan{start, pos_}, String{"非法字符"});
                return;
        }
    }

} // namespace aria

#undef ARIA_CHAR_PRED_REF
