#ifndef ARIA_OBJ_NATIVE_FN_HPP
#define ARIA_OBJ_NATIVE_FN_HPP

#include "aria.hpp"
#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjString;
    class AriaVM;

    // 原生函数的 C++ 调用约定:VM 同步直调、不经字节码帧,slots = [callee, a1..aN],slots[0] 兼返回槽
    //(方法形态下 VM 预先写入接收者)。返回 false 表示已报错(失败载荷留在挂起错误寄存器)。调用期间不得
    // 操作 VM 值栈,新造的对象须先入临时根守卫;切换型原生(resume/yield)不写 slots[0]。
    using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);

    // 原生函数对象:把一个 C++ 函数包装成可在 aria 里调用的值;匿名函数名用 kAnonymousName。
    class ObjNativeFn final : public Object {
    public:
        ObjNativeFn(ObjString* name, NativeFn fn);
        ~ObjNativeFn() override = default; // 无 GC 子内存,壳定长

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        [[nodiscard]]
        NativeFn fn() const noexcept {
            return fn_;
        }

        // mark_object 容 nullptr 仅防御(name_ 本应恒非空)。
        void trace(GC& gc) const noexcept override;

        // 壳定长,无外挂子内存。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjNativeFn);
        }

        // 调试渲染:`<fn name>`(与 ObjFunction 一致;native 身份经 type() 反射可见)。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjString* name_;
        NativeFn   fn_;
    };

    // 工厂:分配 ObjNativeFn;调用方须自行根化 name(fn 是标量)。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, NativeFn fn);

    // 工厂重载(匿名):name 取 kAnonymousName。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, NativeFn fn);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, StringView name, NativeFn fn);

} // namespace aria

#endif // ARIA_OBJ_NATIVE_FN_HPP
