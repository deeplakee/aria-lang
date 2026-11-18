#ifndef ARIA_LEXER_HPP
#define ARIA_LEXER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"
#include "util/utf8.hpp"

namespace aria {

    using src::SourceLoc;

    // 首错即止，返回的 token 借用源缓冲。
    class Lexer {
    public:
        static Result<List<Token>, Error> tokenize(SourceFile& src);

    private:
        static constexpr usize kMaxInterpDepth = 16;

        explicit Lexer(SourceFile& src) noexcept;

        SourceFile& source_; // 借用，扫描期存活
        StringView  src_;    // = source_.content()，'\0' 结尾可作哨兵
        u32         pos_;    // 字节游标
        u32         start_; // 当前扫描单元起点 = 错误锚点（scan_escape 等子扫描器可重指；串级错误经 error_at 显式锚定）
        List<Token> tokens_;
        u32         interp_depth_; // 当前打开的插值档层数（0 = 无嵌套插值，上限 kMaxInterpDepth）

        void run();

        void dispatch_one();

        void skip_trivia();

        [[nodiscard]]
        bool is_line_comment_start(utf8::codepoint cp) const noexcept;

        void scan_number();

        [[nodiscard]]
        Opt<u8> radix_prefix_base() const noexcept;

        void scan_radix_int(u8 base);

        void scan_decimal_or_float();

        // 字符串含 ${ 时在文本态与插值档间切换，无插值时产普通 String token。
        void scan_string(char quote);

        // 返回 true 表示闭串，false 表示遇到 ${；转义仅校验和记账，解码延至消费端。
        bool scan_string_text(char quote, u32 string_start, u32 segment_start, bool has_hole);

        // 仅在 token 边界统计花括号；嵌套字符串和注释由各扫描器整段消费。
        // 返回闭档后下一段文本的起点。
        u32 scan_string_hole(u32 string_start);

        // 入口指向反斜杠，返回展开后的字节数。
        u32 scan_escape();

        // 入口指向 u，返回展开后的字节数。
        u32 scan_unicode_escape();

        void scan_identifier();

        void scan_operator_or_punct();

        // error 锚定 start_，error_at 显式指定位置；异常由 tokenize 捕获。
        template<typename... Args>
        [[noreturn]]
        void error_at(const u32 offset, const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{
                    Error::from_detail(code, loc_at(offset), std::format(fmt, std::forward<Args>(args)...))};
        }

        template<typename... Args>
        [[noreturn]]
        void error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            error_at(start_, code, fmt, std::forward<Args>(args)...);
        }

        // 越界返回 '\0' 哨兵。
        [[nodiscard]]
        char peek_byte(u32 ahead = 0) const noexcept;

        void advance(u32 n = 1) noexcept;

        [[nodiscard]]
        StringView slice(const u32 start, const u32 end) const noexcept {
            ASSERT(start <= end && end <= src_.size(), "invalid slice range");
            return StringView{src_.data() + start, end - start};
        }

        [[nodiscard]]
        SourceLoc loc_at(const u32 offset) const noexcept {
            return SourceLoc{&source_, offset};
        }

        // pred 须拒绝非 ASCII 字节，避免游标停在码点中间；码点扫描用 consume_codepoints。
        template<typename Pred>
        void consume_byte(Pred pred) {
            while (!is_eof() && pred(src_[pos_])) {
                advance();
            }
        }

        template<typename Pred>
        void consume_codepoints(Pred pred) {
            while (!is_eof()) {
                const auto [cp, len] = utf8::decode_one(src_, pos_);
                if (!pred(cp)) {
                    return;
                }
                advance(len);
            }
        }

        [[nodiscard]]
        bool is_eof() const noexcept;
    };

} // namespace aria

#endif // ARIA_LEXER_HPP
