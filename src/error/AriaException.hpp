#ifndef ARIA_EXCEPTION_HPP
#define ARIA_EXCEPTION_HPP

#include <exception>
#include "common.hpp"
#include "error/Error.hpp"
#include "error/ErrorCode.hpp"
#include "util/io.hpp"

namespace aria {

    // aria 解释器内部使用的 C++ 异常基类。
    //
    // 定位：与 Error 值对象 / Result<T, Error> 并存的「另一条错误通道」。
    //   - 可恢复错误：优先 Result<T, Error> 返回（项目约束）。
    //   - 需跨深层调用栈向上传播、又不便层层 Result 的错误：抛 AriaException
    //     及其派生类（如 parser 递归下降深处发现错误时）。
    //   - 不可恢复错误：fatal_error() 直接打印后退出，不走异常。
    //
    // 异常本身只持有一个 Error 对象（复用值类型，不重复表达码/位置/消息）。
    // what() 给出 category:name 形式的 C 字符串，便于在不关心 aria 语义的
    // 通用 catch (std::exception&) 处也能打印。
    //
    // 注意：本类用于「解释器 C++ 实现内部」的错误传播，与 aria 语言自身的
    // throw/catch（抛 Value，由 VM THROW 操作码 + CodeUnit 内异常记录表实现，不引入 SETUP_EXCEPT）无关。
    class AriaException : public std::exception {
    public:
        // 从 Error 值对象构造（唯一构造面：携带码/位置/消息）。不再镜像 Error 的各构造入口 --
        // Error 的组件/成品语义由其静态工厂(from_detail/from_baked)收口,
        // 本类只收成品(Error),镜像会逐入口漂移(见 Error.hpp)。
        explicit AriaException(Error error) : AriaException{std::move(error), make_what(error)} {}

        // 所携错误对象（码/位置/消息）。
        [[nodiscard]]
        const Error& error() const noexcept {
            return error_;
        }

        // std::exception 协议：返回形如 "Syntax: UnterminatedString" 的 C 串。
        // 注意：指向 what_ 内部缓冲，本对象存活期间有效。
        [[nodiscard]]
        const char* what() const noexcept override {
            return what_.c_str();
        }

    protected:
        Error  error_;
        String what_;

        // 生成 what() 用的简短串：category:name（不含位置/消息，避免过长）。
        static String make_what(const Error& e) {
            return std::format("{}: {}", to_string(category_of(e.code())), to_string(e.code()));
        }

    private:
        // 核心构造：what 已由调用方（基于 move 前的 error）算好，此处仅接管。
        // 注意 make_what(error) 必须在 std::move(error) 之前求值--委托构造的
        // 实参求值先于被委托构造体执行，且 make_what 取的是 error 的引用，安全。
        AriaException(Error error, String what) noexcept : error_{std::move(error)}, what_{std::move(what)} {}
    };

    // 编译期异常（词法 / 语法 / 语义阶段）。
    // 仅作为「在编译阶段抛出」的标签，供调用方按阶段 catch；不校验所持 Error 的类别--
    // 编译阶段同样可能遇到 Internal/Resource（如 parser 内部 OOM、不可达分支），这些码
    // 也可合法地由此异常承载。阶段与码并非强绑定，靠开发者按场景选用，不靠运行时校验。
    class AriaCompileException : public AriaException {
    public:
        explicit AriaCompileException(Error error) : AriaException{std::move(error)} {}
    };

    // 运行期异常（VM 执行期间）。
    // 仅作为「在运行阶段抛出」的标签，供调用方按阶段 catch；不校验所持 Error 的类别--
    // 运行阶段也可能遇到 Internal/Resource（如 VM 内部 Unreachable、栈溢出）。阶段与码
    // 并非强绑定，靠开发者按场景选用，不靠运行时校验。
    class AriaRuntimeException : public AriaException {
    public:
        explicit AriaRuntimeException(Error error) : AriaException{std::move(error)} {}
    };

} // namespace aria

#endif // ARIA_EXCEPTION_HPP
