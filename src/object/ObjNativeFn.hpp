#ifndef ARIA_OBJ_NATIVE_FN_HPP
#define ARIA_OBJ_NATIVE_FN_HPP

#include "aria.hpp"
#include "common.hpp"
#include "object/Object.hpp"
#include "runtime/NativeCallInfo.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjString;

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
        String repr() const override;

        // 从所属类载入:自 NativeFn 类链起查(本类无值本地状态)。
        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 裸读 override:类链直取,miss 就地 fail;绑定读走基类默认(原值自身即槽 0 接收者)。
        [[nodiscard]]
        Opt<Value> load_field(AriaExeContext& ctx, ObjString* name) override;

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
