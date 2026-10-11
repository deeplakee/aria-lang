#ifndef ARIA_NATIVE_CALL_INFO_HPP
#define ARIA_NATIVE_CALL_INFO_HPP

#include "common.hpp"
#include "runtime/AriaExeContext.hpp"
#include "value/Value.hpp"

namespace aria {

    class NativeCallInfo;

    // 原生函数的 C++ 调用约定:VM 同步直调、不经字节码帧,调用区 [self, a1..aN] 全程留栈(善后
    // drop 在原生返回后才发生)。原生经 NativeCallInfo 读 self 与实参、写返回值,失败返 false
    //(载荷已在挂起错误寄存器);切换型原生(resume/yield)不写返回槽。
    using NativeFn = bool (*)(const NativeCallInfo& info);

    // 一次原生调用的操作面板:self 与实参只读、返回值经 set_return_val 写回。持槽 0 在值栈缓冲
    // 中的下标而非指针,每次访问经 ctx 重取基址按偏移重算,原生执行期间的重入扩容(grow_stack_
    // 搬迁)不再悬垂。
    class NativeCallInfo {
    public:
        // 调用区须已在栈上:栈顶 argc+1 个值恰为 [self, a1..argc]。
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

        // 槽 0:普通调用 = 闭包自身,方法调用 = receiver;set_return_val 惯例为返回前最后动作,
        // 其后读到的是返回值。
        [[nodiscard]]
        Value self() const noexcept {
            return slot(0);
        }

        // 实参只读访问,0 起不含槽 0;按值返回,跨重入持有安全(对象经栈根存活)。
        [[nodiscard]]
        Value operator[](const usize index) const noexcept {
            ASSERT(index < argc_, "argument index out of range");
            return slot(index + 1);
        }

        // 写返回槽(即槽 0);切换型原生(resume/yield)不写,槽 0 归对侧作预留结果槽。
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
