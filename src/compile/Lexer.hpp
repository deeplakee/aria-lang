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
        // 字符串模板插值嵌套深度上限。
        static constexpr usize kMaxInterpDepth = 16;

        explicit Lexer(SourceFile& src) noexcept;

        SourceFile& source_; // 借用，扫描期存活
        StringView  src_;    // = source_.content()，'\0' 结尾可作哨兵
        u32         pos_;    // 字节游标
        u32         start_; // 当前扫描单元起点 = 错误锚点（scan_escape 等子扫描器可重指；串级错误经 error_at 显式锚定）
        List<Token> tokens_;
        u32         interp_depth_; // 当前打开的插值档层数（0 = 无嵌套插值，上限 kMaxInterpDepth）

        void run();

        // 单步分派：产一个普通 token（或吞一段 trivia）。run 主循环与插值档内扫描共用。
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

        // 字符串字面量（含转义校验）；字符串即模板：${ 开插值档（嵌套经 dispatch_one 递归，深度上限
        // kMaxInterpDepth），裸 $ 与 { } 皆普通字符，无档整串退化普通 String token。状态机驱动：
        // 文本态与档内态交替至闭串。
        void scan_string(char quote);

        // 文本态：扫一段字面（含转义校验与展开长度记账）产段 token，段原文只以 lexeme 随 token 走、
        // 解码延至消费端；返回 true = 闭串，false = ${ 开档（档内 token 由 scan_string_hole 产出）。
        // 段 token 与串级错误分别锚 segment_start / string_start。
        bool scan_string_text(char quote, u32 string_start, u32 segment_start, bool has_hole);

        // 档内态：花括号配对内的普通 token 流；预检位恒为 token 边界（串内/注释/嵌套模板里的 { } 已被
        // 整段消费不会到此），{ 加深度、非归零 } 减深度照常产 token（map/lambda 体属表达式），深度归零
        // 的 } 闭档。返回闭档后的偏移 = 下一段字面原文起点（scan_string 以此推进 segment_start）。
        u32 scan_string_hole(u32 string_start);

        // 串内转义校验（入口 pos_ 指向 '\\'）：转义集合与展开形态的校验全在词法期收口（首错即止），
        // 解码延至消费端（util/str.hpp）；返回该转义展开后的字节数。
        u32 scan_escape();

        // \u{...} Unicode 转义（入口 pos_ 指向 'u'）：解析与校验收口 util/str.hpp 的
        // decode_unicode_escape（唯一解析口），此处只接线失败报错与推进记账，返回展开后字节数。
        u32 scan_unicode_escape();

        // identifier / keyword / _
        void scan_identifier();

        // 运算符 / 标点（最长匹配）；非 ASCII 码点记 InvalidCharacter
        void scan_operator_or_punct();

        // 记错即抛 AriaCompileException（tokenize 顶层 catch 翻译为 Result）。报错点自陈位置：error 锚
        // start_，error_at 显式锚任意偏移（串级错误锚串起点），后者免去「重指后由调用方恢复」的隐性契约。
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

        // 推进游标 n 字节；越界断言（pos_ + n <= src_.size()），多字节推进统一走此。
        void advance(u32 n = 1) noexcept;

        // [start, end) 半开区间截取 src_ 的字节视图，两参均为字节偏移。
        [[nodiscard]]
        StringView slice(const u32 start, const u32 end) const noexcept {
            ASSERT(start <= end && end <= src_.size(), "invalid slice range");
            return StringView{src_.data() + start, end - start};
        }

        // offset 处的源位置（SourceLoc 构造收口，词根对齐 error_at）。
        [[nodiscard]]
        SourceLoc loc_at(const u32 offset) const noexcept {
            return SourceLoc{&source_, offset};
        }

        // 按裸字节消费（pred 收 char）。谓词须自证 ASCII（对 >= 0x80 字节为假，如数字、字符串分隔符），
        // 游标才不停在码点中间；按码点消费（标识符续接等）用 consume_codepoints。
        template<typename Pred>
        void consume_byte(Pred pred) {
            while (!is_eof() && pred(src_[pos_])) {
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
