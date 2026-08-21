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

    // 错误值对象：聚合 ErrorCode + SourceLoc(可空) + 可读消息，作为各阶段统一错误载体。
    //
    // 设计要点：
    //   - 值类型（可拷贝/移动），供 Result<T, Error> 携带，符合项目「错误处理倾向
    //     Result 返回而非抛 C++ 异常」的约束。
    //   - 只做「承载 + 渲染」：分类/名称/相等判定等由所持 ErrorCode 提供（经 code()
    //     再取），Error 不重复暴露委托方法。
    //   - loc 为 SourceLoc：词法/语法/语义错误通常带位置；部分内部/资源错误无位置
    //     （用 SourceLoc 默认构造的空态，src=nullptr，to_string 返回 "?"）。SourceLoc 把
    //     「源文件指针 + 行列」收口为一处，空态即「无位置」。
    //   - message 为人类可读细节（含变量名、期待 token 等）；to_string(code()) 给机器标识。
    //
    // 生命周期约束：loc_ 内的 src 指针指向的 SourceFile 不得比本 Error 活得更久
    // （同 Token::lexeme_ 的 StringView 约束）。多数情况下 Error 在编译/解释同一源
    // 文件期间产生并消费，SourceFile 存活于整个流程，安全；跨流程传递 Error 时需另行保证。
    //
    // 注意：本类只承载「解释器报告的错误」。aria 语言自身的 throw/catch 抛的是 Value，
    // 由 VM 用 THROW 操作码 + CodeUnit 内异常记录表实现（不引入 SETUP_EXCEPT，见
    // CLAUDE.md「错误处理」第 2 条），与 C++ 异常无关，不经过本类。
    class Error {
    public:
        // 无位置错误：仅码 + 可选消息。供内部/资源错误或不关心位置的场景使用。
        // ErrorCode 为普通 enum class，故常见写法 Error{ErrorCode::X, "msg"}。
        Error(ErrorCode code, String message = {}) : code_{code}, loc_{}, message_{std::move(message)} {}

        // 带位置错误：码 + SourceLoc + 可选消息。供词法/语法/语义阶段使用。
        Error(ErrorCode code, SourceLoc loc, String message = {}) :
            code_{code}, loc_{loc}, message_{std::move(message)} {}

        // 所属错误码（分类/名称/相等判定经此再取）。
        [[nodiscard]]
        ErrorCode code() const noexcept {
            return code_;
        }

        // 源码位置（可能为空态--内部/资源错误或无需定位的场景；空态为默认构造的 SourceLoc，
        // src 为 nullptr，to_string 返回 "?"）。
        [[nodiscard]]
        const SourceLoc& loc() const noexcept {
            return loc_;
        }

        // 人类可读细节消息。
        [[nodiscard]]
        const String& message() const noexcept {
            return message_;
        }

        // 拼接为单行可读串，位置信息前置（符合编译器报错惯例）：
        //   有 SourceLoc ："main.aria:3:5: Syntax: UnterminatedString 字符串未闭合"
        //   无 SourceLoc ："Syntax: UnterminatedString 字符串未闭合"
        [[nodiscard]]
        String format() const {
            String prefix;
            if (loc_.source() != nullptr) { // 空态 SourceLoc(src=nullptr)即「无位置」
                prefix = std::format("{}: ", loc_.to_string());
            }
            String out = std::format("{}{}: {}", prefix, to_string(category_of(code_)), to_string(code_));
            if (!message_.empty()) {
                out += ' ';
                out += message_;
            }
            return out;
        }

    private:
        ErrorCode code_;
        SourceLoc loc_;
        String    message_;
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
        io::println(stderr, "aria: fatal: {}", error.format());
        std::exit(1);
    }

    // 便捷重载：仅码 + 可选消息（无位置）。
    [[noreturn]]
    inline void fatal_error(ErrorCode code, String message = {}) {
        fatal_error(Error{code, std::move(message)});
    }

    // 便捷重载：码 + 位置 + 可选消息。
    [[noreturn]]
    inline void fatal_error(ErrorCode code, SourceLoc loc, String message = {}) {
        fatal_error(Error{code, loc, std::move(message)});
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
