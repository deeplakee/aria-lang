#ifndef ARIA_NATIVE_CALL_INFO_HPP
#define ARIA_NATIVE_CALL_INFO_HPP

#include "common.hpp"
#include "error/ErrorCode.hpp"
#include "object/Object.hpp"
#include "runtime/AriaExeContext.hpp"
#include "value/Value.hpp"

namespace aria {

    class NativeCallInfo;

    // 调用区 [self, a1..aN] 留栈保活；返回 false 时须已挂起错误。
    // resume/yield 成功后切换上下文，不写返回槽。
    using NativeFn = bool (*)(const NativeCallInfo& info);

    // 以槽下标定位调用区，每次访问重取基址，允许原生调用期间重入并搬迁值栈。
    class NativeCallInfo {
    public:
        NativeCallInfo(AriaExeContext& ctx, const u8 argc) : ctx_{&ctx}, argc_{argc} {
            ASSERT(ctx.stack_size() >= argc + 1, "call region not on stack");
            base_index_ = ctx.stack_size() - argc - 1;
        }

        [[nodiscard]]
        AriaExeContext& ctx() const noexcept {
            return *ctx_;
        }

        [[nodiscard]]
        u8 size() const noexcept {
            return argc_;
        }

        // self 与返回值共用槽 0，写返回值须留到最后。
        [[nodiscard]]
        Value self() const noexcept {
            return slot(0);
        }

        // 类上取出的裸方法可被任意接收者调用，类型不匹配属于语言错误。
        template<DerivedFromObj T>
        [[nodiscard]]
        T* receiver() const {
            constexpr auto tag  = obj_tag<T>();
            const Value    self = slot(0);
            if (self.is_obj() && self.as_obj()->type() == tag.tag) {
                return self.as_obj()->as<T>();
            }
            return ctx_->fail(ErrorCode::TypeMismatch, "receiver must be a {}, got {}", tag.word, type_name(self));
        }

        [[nodiscard]]
        Value operator[](const usize index) const noexcept {
            ASSERT(index < argc_, "argument index out of range");
            return slot(index + 1);
        }

        void set_return_val(const Value value) const noexcept { slot(0) = value; }

    private:
        [[nodiscard]]
        Value& slot(const usize offset) const noexcept {
            return ctx_->stack_base()[base_index_ + offset];
        }

        AriaExeContext* ctx_;
        usize           base_index_;
        u8              argc_;
    };

} // namespace aria

#endif // ARIA_NATIVE_CALL_INFO_HPP
