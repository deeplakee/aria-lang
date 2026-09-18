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
    // 静态服务入口 tokenize(SourceFile&)：内部一次性构造（私有构造），扫完即销毁--
    // 无空态、无复用。源文件按借用约定传引用（见 CPP_Naming_Convention.md Parameter
    // Passing），扫描期间须存活。
    //
    // 返回 Result<List<Token>, List<Error>>：
    //   - 所有词法错误（串未闭合等）一律作可恢复处理--
    //     记入 List<Error> 后推进 pos_ 继续扫描，尽量多收集错误，而非遇首个错误即终止。
    //   - 为缓解级联错误设错误上限 kMaxErrors，达上限即停。
    //   - 有任何错误 -> unexpected(List<Error>)；无错 -> 完整 token 流（含末尾 Eof）。
    //
    // 扫描基于 utf8::decode_one 按码点推进；src_ 底层 String 以 '\0' 结尾，可作哨兵。
    //
    // 位置模型：token 位置 = 起点字节偏移（扫描期间已有的 start），行列由 SourceLoc 在消费点派生
    // （成本契约见 source_file.hpp）--词法期不维护任何行列计数，故游标推进就是普通的字节推进，
    // 回退/前瞻也不需要还原状态。
    class Lexer {
    public:
        // 对 src 做词法分析，返回 token 流或错误集合。
        static Result<List<Token>, List<Error>> tokenize(SourceFile& src);

    private:
        // 错误上限：errors_ 达此数即置 is_fatal_ 停止扫描，避免级联刷屏。
        static constexpr usize kMaxErrors = 32;

        // 一次性实例：构造即注入扫描状态，仅静态入口 tokenize 构造。
        explicit Lexer(SourceFile& src) noexcept;

        // 扫描状态（构造注入）
        SourceFile& source_; // 借用，扫描期存活
        StringView  src_;    // = source_.content()，'\0' 结尾可作哨兵
        u32         pos_;    // 字节游标
        List<Token> tokens_;
        List<Error> errors_;
        bool        is_fatal_; // 错误达上限，主循环应终止

        // --- 主循环与分支 ---
        // 主扫描循环
        void run();
        // 跳过空白 + // / # 注释
        void skip_trivia();
        // 数字：分流 radix / decimal-or-float
        void scan_number();
        // 0b/0o/0x 整数（入口 pos_ 即起点）
        void scan_radix_int();
        // decimal / int 指数 / float（入口 pos_ 即起点）
        void scan_decimal_or_float();
        // plainString（含转义解析）
        void scan_string();
        // 串内转义解析（\" \' \\ n t r 0 \u{hex+}）
        void scan_escape(String& value);
        // identifier / keyword / _
        void scan_identifier();
        void scan_operator_or_punct(utf8::codepoint cp);


        // --- 错误记账 ---
        // 记入一条错误：span 起点解析为 SourceLoc，经 Error::from_detail 构造期烘进 message_。
        void error(ErrorCode code, SourceSpan span, StringView msg);

        // --- 游标辅助 ---
        // 越界（pos_+ahead >= src_.size()）返回 '\0' 哨兵，安全。
        [[nodiscard]]
        char peek_byte(u32 ahead = 0) const noexcept;

        // 推进游标 n 字节。含越界断言（pos_ + n <= src_.size()），调试期捕获推进过头。
        // 多字节推进（进制前缀 / 多字符运算符 / decode 长度）统一走此。
        void advance(u32 n = 1) noexcept;

        // 当 pred(当前码点首字节) 为真且未到 EOF 时，逐码点推进 pos_，直至 pred 假或 EOF。用于
        // 「连续消费满足某谓词的码点」循环（如数字序列、注释到行尾、\u{...} 收集 hex）--谓词按首
        // 字节判定、多字节码点整吞一步，故游标不会停在码点中间。循环体需额外副作用（如设标志）
        // 的场景不适用，仍手写循环。
        template<typename Pred>
        void conditional_advance(Pred pred) {
            while (!is_eof() && pred(src_[pos_])) {
                advance(utf8::decode_one(src_, pos_).second);
            }
        }

        [[nodiscard]]
        bool is_eof() const noexcept;
    };

} // namespace aria

#endif // ARIA_LEXER_HPP
