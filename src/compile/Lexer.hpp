#ifndef ARIA_LEXER_HPP
#define ARIA_LEXER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"
#include "util/utf8.hpp"

namespace aria {

    using src::SourceLoc;

    // 词法分析器：把 SourceFile 内容切成 Token 流。一次性实例（私有构造，扫完即销毁）。
    // 词法错误一律可恢复收集（达 kMaxErrors 停），非首错即止；token 位置 = 起点字节偏移，
    // 行列由 SourceLoc 在消费点派生（词法期不维护行列计数，回退/前瞻无需还原状态）。
    class Lexer {
    public:
        static Result<List<Token>, List<Error>> tokenize(SourceFile& src);

    private:
        // 错误上限：errors_ 达此数即置 is_fatal_ 停止扫描，避免级联刷屏。
        static constexpr usize kMaxErrors = 32;

        explicit Lexer(SourceFile& src) noexcept;

        SourceFile& source_; // 借用，扫描期存活
        StringView  src_;    // = source_.content()，'\0' 结尾可作哨兵
        u32         pos_;    // 字节游标
        List<Token> tokens_;
        List<Error> errors_;
        bool        is_fatal_; // 错误达上限，主循环应终止

        void run();
        // 跳过空白 + // / # 注释
        void skip_trivia();
        // 数字：分流 radix / decimal-or-float
        void scan_number();
        // 0b/0o/0x 整数（入口 pos_ 即起点）
        void scan_radix_int();
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


        // 记入一条错误，offset 解析为 SourceLoc。
        void error(ErrorCode code, StringView msg, u32 offset);

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
