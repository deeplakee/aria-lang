#ifndef ARIA_LEXER_HPP
#define ARIA_LEXER_HPP

#include "common.hpp"
#include "compile/Token.hpp"
#include "error/Error.hpp"
#include "util/source_file.hpp"

namespace aria {

    // 将 SourceSpan / SourceLoc 引入 aria 命名空间（见 CLAUDE.md：source_file 相关
    // 类型位于 aria::src 下，引用需分别 using）。
    using src::SourceLoc;
    using src::SourceSpan;

    // 词法分析器：把 SourceFile 的内容切成 Token 流。
    //
    // 生命周期：默认构造为空态；通过 tokenize(const SourceFile&) 传入源文件、
    // 初始化成员、扫描、扫完清空成员返回。Lexer 可复用（多次 tokenize 不同文件），
    // 故以指针持有 SourceFile（nullptr 表空态，引用无法「清空」）。
    //
    // 返回 Result<List<Token>, List<Error>>：
    //   - 所有词法错误（串未闭合等）一律作可恢复处理--
    //     记入 List<Error> 后推进 pos_ 继续扫描，尽量多收集错误，而非遇首个错误即终止。
    //   - 为缓解级联错误，设错误上限 kMaxErrors：errors_ 达上限即置 is_fatal_，
    //     主循环检测后停止，避免无限级联刷屏。
    //   - 有任何错误 -> unexpected(List<Error>)；无错 -> 完整 token 流（含末尾 Eof）。
    //
    // 扫描基于 utf8::decode_one 按码点推进；src_ 底层 String 以 '\0' 结尾，可作哨兵。
    // 复用 Token 工厂（make_integer/make_float/make_string）、lookup_keyword、
    // utf8::is_id_start/is_id_continue/is_whitespace/is_digit。
    class Lexer {
    public:
        // 空态构造：成员全空，待 tokenize 注入 SourceFile。
        Lexer() noexcept;

        // 对 src 做词法分析。扫完清空成员。返回 token 流或错误集合。
        [[nodiscard]]
        Result<List<Token>, List<Error>> tokenize(SourceFile* src);

    private:
        // 错误上限：errors_ 达此数即置 is_fatal_ 停止扫描，避免级联刷屏。
        static constexpr usize kMaxErrors = 32;

        // 扫描状态（tokenize 注入，扫完清空）
        SourceFile* source_; // nullptr 表空态；扫描期指向 src
        StringView  src_;    // = source_->content()，'\0' 结尾可作哨兵
        usize       pos_;    // 字节游标
        List<Token> tokens_;
        List<Error> errors_;
        bool        is_fatal_; // 错误达上限，主循环应终止

        // --- 主循环与分支 ---
        void run();                      // 主扫描循环
        void skip_trivia();              // 跳过空白 + // / # 注释
        void scan_number();              // 数字：分流 radix / decimal-or-float
        void scan_radix_int();           // 0b/0o/0x 整数（入口 pos_ 即起点）
        void scan_decimal_or_float();    // decimal / int 指数 / float（入口 pos_ 即起点）
        void scan_string();              // plainString（含转义解析）
        void scan_escape(String& value); // 串内转义解析（\" \' \\ n t r 0 \u{hex+}）
        void scan_identifier();          // identifier / keyword / _
        void scan_operator_or_punct(utf8::codepoint cp);


        // --- 错误记账 ---
        // 记入 errors_。若达 kMaxErrors 则置 is_fatal_，主循环将停止。
        void error(ErrorCode code, SourceSpan span, String msg);

        // 构造带 source_ 的 Error（format() 可输出 path:line:col）。
        [[nodiscard]]
        Error make_error(ErrorCode code, SourceSpan span, String msg) const;

        // --- 游标辅助 ---
        // 越界（pos_+ahead >= src_.size()）返回 '\0' 哨兵，安全。
        [[nodiscard]]
        char peek_byte(usize ahead = 0) const noexcept;

        // 前瞻码点（不推进 pos_）。用于标识符/数字前瞻。
        [[nodiscard]]
        utf8::codepoint peek_codepoint(usize ahead = 0) const noexcept;

        // 推进游标 n 字节。含越界断言（pos_+n <= src_.size()），调试期捕获推进过头。
        // 多字节推进（+= 2/+= 3/+= len）统一走此；单字节 ++pos_ 循环内可保留。
        void advance(usize n = 1) noexcept;

        // 当 pred(当前字节) 为真且未到 EOF 时，逐字节推进 pos_，直至 pred 假或 EOF。
        // 用于「连续消费满足某谓词的字节」循环（如数字序列、注释到行尾、\u{...} 收集 hex）。
        // 循环体需额外副作用（如设标志）的场景不适用，仍手写循环。
        template<typename Pred>
        void conditional_advance(Pred pred) {
            while (!is_eof() && pred(src_[pos_])) {
                ++pos_;
            }
        }

        [[nodiscard]]
        bool is_eof() const noexcept;

        // 把字节偏移解析为 SourceLoc（token 位置构造用）。
        [[nodiscard]]
        SourceLoc loc_at(usize offset) const;
    };

} // namespace aria

#endif // ARIA_LEXER_HPP
