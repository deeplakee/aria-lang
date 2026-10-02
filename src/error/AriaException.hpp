#ifndef ARIA_EXCEPTION_HPP
#define ARIA_EXCEPTION_HPP

#include "common.hpp"
#include "error/Error.hpp"

namespace aria {

    // 解释器内部使用的 C++ 异常基类:与 Result<T, Error> 并存的跨深层调用栈错误通道(如 parser 深递归找错)。
    // 只持一个 Error 经 error() 取用,不挂 std::exception;与语言自身 throw/catch(走 VM 异常通道)无关。
    class AriaException {
    public:
        // 从 Error 值对象构造（唯一公开构造面：只收成品 Error，组件构造由 Error 静态工厂收口）。
        explicit AriaException(Error error) noexcept : error_{std::move(error)} {}

        [[nodiscard]]
        const Error& error() const noexcept {
            return error_;
        }

    protected:
        Error error_;
    };

    // 编译期异常（词法 / 语法 / 语义阶段）：仅作阶段标签供按阶段 catch；不校验类别（阶段与码不强绑定）。
    class AriaCompileException : public AriaException {
    public:
        explicit AriaCompileException(Error error) noexcept : AriaException{std::move(error)} {}
    };

    // 运行期异常（VM 执行期间）：阶段标签，同样不校验类别、不强绑定。
    class AriaRuntimeException : public AriaException {
    public:
        explicit AriaRuntimeException(Error error) noexcept : AriaException{std::move(error)} {}
    };

} // namespace aria

#endif // ARIA_EXCEPTION_HPP
