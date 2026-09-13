#ifndef ARIA_EXCEPTION_HPP
#define ARIA_EXCEPTION_HPP

#include "common.hpp"
#include "error/Error.hpp"

namespace aria {

    // aria 解释器内部使用的 C++ 异常基类：与 Error 值对象 / Result<T, Error> 并存的另一条
    // 错误通道，用于需跨深层调用栈向上传播、又不便层层 Result 的错误（如 parser 递归下降
    // 深处发现错误时）；不可恢复错误走 fatal_error()，不走异常。
    // 异常本身只持有一个 Error 对象（复用值类型，不重复表达码/位置/消息），
    // 消费方经 error() 取用，不挂 std::exception 协议面。
    //
    // 注意：仅用于「解释器 C++ 实现内部」的错误传播，与 aria 语言自身的 throw/catch
    // （抛 Value，走 VM 异常通道）无关。四通道总览见 CLAUDE.md「错误处理」。
    class AriaException {
    public:
        // 从 Error 值对象构造（唯一公开构造面：只收成品 Error；组件构造由 Error 静态工厂收口，
        // 镜像其各构造入口会随构造面漂移）。
        explicit AriaException(Error error) noexcept : error_{std::move(error)} {}

        [[nodiscard]]
        const Error& error() const noexcept {
            return error_;
        }

    protected:
        Error error_;
    };

    // 编译期异常（词法 / 语法 / 语义阶段）：仅作阶段标签供调用方按阶段 catch；
    // 不校验所持 Error 的类别（编译阶段同样可能遇到 Internal/Resource），阶段与码不强绑定。
    class AriaCompileException : public AriaException {
    public:
        explicit AriaCompileException(Error error) noexcept : AriaException{std::move(error)} {}
    };

    // 运行期异常（VM 执行期间）：阶段标签，语义同 AriaCompileException（不校验类别、不强绑定）。
    class AriaRuntimeException : public AriaException {
    public:
        explicit AriaRuntimeException(Error error) noexcept : AriaException{std::move(error)} {}
    };

} // namespace aria

#endif // ARIA_EXCEPTION_HPP
