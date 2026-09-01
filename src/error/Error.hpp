#ifndef ARIA_ERROR_HPP
#define ARIA_ERROR_HPP

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "util/io.hpp"
#include "util/source_file.hpp"

namespace aria {

    // 将 source_file 相关类型引入 aria 命名空间（见 CLAUDE.md：这些类型位于
    // aria::src 下，引用需分别 using）。
    using src::SourceFile;
    using src::SourceLoc;
    using src::SourceSpan;

    // 错误值对象：聚合 ErrorCode + 完整可读消息，作为各阶段统一错误载体。
    //
    // 设计要点：
    //   - 值类型（可拷贝/移动），供 Result<T, Error> 携带，符合项目「错误处理倾向
    //     Result 返回而非抛 C++ 异常」的约束。
    //   - 只两个字段：code_（机器标识，供分类/名称/相等判定经 code() 再取）+ message_
    //     （完整人类可读串）。message_ 在构造期一次性烘焙成型--把 SourceLoc 的
    //     "path:line:col"、分类名、码名、细节拼成最终串存下，构造完成后 Error 完全自有、
    //     不持任何 SourceFile* 裸指针，可任意拷贝/移动/跨流程传递，无悬空风险。
    //   - 不保留结构化位置（LineCol/SourceLoc）字段：解释器无需多错误按位置排序/去重等
    //     能力，结构化位置只会徒增复杂度与生命期约束。需要位置时直接读 message_ 串即可。
    //
    // 生命周期：位置在构造期一次性格式化（此时 SourceFile 必然存活），构造完成后 Error 与
    // SourceFile 完全解耦。这与 Token::lexeme_ 仍借 StringView 指向 SourceFile::content_
    // （需 SourceFile 存活）的约束不同：Error 不再有此类约束。
    //
    // 注意：本类只承载「解释器报告的错误」。aria 语言自身的 throw/catch 抛的是 Value，
    // 由 VM 用 THROW 操作码 + CodeUnit 内异常记录表实现（不引入 SETUP_EXCEPT，见
    // CLAUDE.md「错误处理」第 2 条），与 C++ 异常无关，不经过本类。
    class Error {
    public:
        // 无位置错误：码 + 可选细节。供内部/资源错误或不关心位置的场景使用。
        // ErrorCode 为普通 enum class，故常见写法 Error{ErrorCode::X, "detail"}。
        // message_ 烘为 "Category: Name[ detail]"。
        Error(ErrorCode code, const String& detail = "") : code_{code}, message_{make_message(code, nullptr, detail)} {}

        // 带位置错误：码 + SourceLoc + 可选细节。供词法/语法/语义阶段使用。
        // 构造期就地用 loc 烘位置前缀（SourceFile 此刻存活，安全），此后不再持有
        // SourceLoc/SourceFile*。空态 loc(source()==nullptr)按「无位置」处理。
        // message_ 烘为 "path:line:col: Category: Name[ detail]"。
        Error(ErrorCode code, const SourceLoc& loc, const String& detail = "") :
            code_{code}, message_{make_message(code, &loc, detail)} {}

        // 所属错误码（分类/名称/相等判定经此再取）。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return code_;
        }

        // 完整可读消息（构造期烘焙成型，含位置前缀 + 分类名 + 码名 + 细节）。
        // 形如 "main.aria:3:5: Syntax: UnterminatedString 字符串未闭合"（带位置）或
        // "Syntax: UnterminatedString 字符串未闭合"（无位置）。自存、不依赖任何外部对象。
        [[nodiscard]]
        const String& message() const noexcept {
            return message_;
        }

    private:
        ErrorCode code_;
        String    message_;

        // 烘焙完整消息串：[loc 前缀 + ": "] + "Category: Name"[ + " " + detail]。
        // loc 为 nullptr 或 loc->source()==nullptr 时无位置前缀。detail 空则无细节尾。
        static String make_message(ErrorCode code, const SourceLoc* loc, const String& detail) {
            String s;
            if (loc != nullptr && loc->source() != nullptr) {
                s = std::format("{}: ", loc->to_string()); // "path:line:col: "
            }
            s += std::format("{}: {}", to_string(category_of(code)), to_string(code));
            if (!detail.empty()) {
                s += ' ';
                s += detail;
            }
            return s;
        }
    };

    // 不可恢复错误：打印错误到 stderr 后以退出码 1 终止进程。
    //
    // 适用场景：解释器自身不变式被破坏（Internal 类，如 Unreachable /
    // InvalidBytecode）或资源耗尽（Resource 类，如 OutOfMemory）--这类错误
    // 不应靠返回值层层传播，直接终止更安全。函数标记 [[noreturn]]。
    //
    // 与抛异常的区别：fatal_error 不抛、不做栈展开，打印后立即 std::exit，
    // 适用于「无法继续执行」的致命错误；可恢复或可跨栈传播的错误用
    // Result<T, Error> / AriaException。
    [[noreturn]]
    inline void fatal_error(const Error& error) {
        io::println(stderr, "aria: fatal: {}", error.message());
        std::exit(1);
    }

    // 便捷重载：仅码 + 可选消息（无位置）。
    [[noreturn]]
    inline void fatal_error(const ErrorCode code, const String& message = "") {
        fatal_error(Error{code, message});
    }

    // 便捷重载：码 + 位置 + 可选消息。
    [[noreturn]]
    inline void fatal_error(const ErrorCode code, const SourceLoc& loc, const String& message = "") {
        fatal_error(Error{code, loc, message});
    }

    // 便捷工厂：以 std::format 风格直接格式化构造 Error（无位置）。供各阶段报错用，
    // 避免调用方手写 Error{code, std::format(...)} 的嵌套样板。带位置版本直接用构造函数。
    // 命名取 errorf（f=format，类比 printf/fprintf）：Error 是值类型，不走 GC 对象工厂
    // （new_string/new_function/new_object 等），故不取 make_/new_ 工厂前缀以免混淆。
    template<typename... Args>
    [[nodiscard]]
    Error errorf(ErrorCode code, std::format_string<Args...> fmt, Args&&... args) {
        return Error{code, std::format(fmt, std::forward<Args>(args)...)};
    }

} // namespace aria

#endif // ARIA_ERROR_HPP
