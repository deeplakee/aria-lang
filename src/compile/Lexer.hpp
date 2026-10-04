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

    // 词法分析器：把 SourceFile 内容切成 Token 流。一次性实例（私有构造，扫完即销毁）。
    // 词法错误首错即止：error() 抛 AriaCompileException，tokenize 顶层 catch 翻译为 Result；
    // token 位置 = 起点字节偏移，行列由 SourceLoc 在消费点派生（词法期不维护行列计数，回退/前瞻无需还原状态）。
    class Lexer {
    public:
        static Result<List<Token>, Error> tokenize(SourceFile& src);

    private:
        explicit Lexer(SourceFile& src) noexcept;

        SourceFile& source_; // 借用，扫描期存活
        StringView  src_;    // = source_.content()，'\0' 结尾可作哨兵
        u32         pos_;    // 字节游标
        u32         start_;  // 当前扫描单元起点 = 错误锚点（子扫描器可重指，调用方恢复）
        List<Token> tokens_;

        void run();

        // 单步分派：产一个普通 token（或吞一段 trivia）。run 主循环与后续的插值档内扫描共用。
        void dispatch_one();

        // 跳过空白 + // / # 注释
        void skip_trivia();

        // 当前位是否行注释起点（// 或 #，均到行尾）。
        [[nodiscard]]
        bool is_line_comment_start(utf8::codepoint cp) const noexcept;

        // 数字：分流 radix / decimal-or-float
        void scan_number();

        // 当前位是否进制前缀（0b/0o/0x，大小写均可）：是则返回进制数（2/8/16），否则 nullopt。
        [[nodiscard]]
        Opt<u8> radix_prefix_base() const noexcept;

        // 0b/0o/0x 整数（入口 pos_ 即起点，base 由 scan_number 经 radix_prefix_base 判定传入）
        void scan_radix_int(u8 base);

        // decimal / int 指数 / float（入口 pos_ 即起点）
        void scan_decimal_or_float();

        // 字符串字面量（含转义解析）
        void scan_string();

        // 串内转义解析
        void scan_escape(String& value);

        // identifier / keyword / _
        void scan_identifier();

        // 运算符 / 标点（最长匹配）；非 ASCII 码点记 InvalidCharacter
        void scan_operator_or_punct();

        // 记错即抛 AriaCompileException（tokenize 顶层 catch 翻译为 Result）；位置锚 start_。
        template<typename... Args>
        [[noreturn]]
        void error(const ErrorCode code, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{Error::from_detail(code, SourceLoc{&source_, start_},
                                                          std::format(fmt, std::forward<Args>(args)...))};
        }

        // 越界返回 '\0' 哨兵。
        [[nodiscard]]
        char peek_byte(u32 ahead = 0) const noexcept;

        // 推进游标 n 字节；越界断言（pos_ + n <= src_.size()），多字节推进统一走此。
        void advance(u32 n = 1) noexcept;

        // 只消费 ASCII 字节（pred 收 char），遇非 ASCII 必停且停在码点边界（UTF-8 续接字节恒 >= 0x80）；
        // 含非 ASCII 字符集的序列（标识符续接等）须用 consume_codepoints。
        template<typename Pred>
        void consume_ascii(Pred pred) {
            consume_u8([pred](const u8 byte) { return utf8::is_ascii(byte) && pred(static_cast<char>(byte)); });
        }

        // 按裸字节消费（pred 收 u8），不保证停在码点边界--谓词对 >= 0x80 字节为真时游标停进码点中间；
        // 只用于停点必为 ASCII 的场景（字符串正文按 ASCII 分隔符切段），需边界保证用 consume_ascii。
        template<typename Pred>
        void consume_u8(Pred pred) {
            while (!is_eof() && pred(static_cast<u8>(src_[pos_]))) {
                advance();
            }
        }

        // 消费满足 pred 的连续码点（pred 收码点），整码点推进，游标不停在码点中间。
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
