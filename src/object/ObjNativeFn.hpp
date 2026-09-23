#ifndef ARIA_OBJ_NATIVE_FN_HPP
#define ARIA_OBJ_NATIVE_FN_HPP

#include "aria.hpp"   // kAnonymousName
#include "common.hpp" // Span / u8 / String (经 type.hpp)
#include "object/Object.hpp"
#include "value/Value.hpp" // Value

namespace aria {

    class GC;
    class ObjString;
    class AriaVM; // 前向声明:NativeFn 形参取引用,本头不依赖其完整定义

    // 原生函数的 C++ 调用签名(VM 在 CALL 命中 ObjNativeFn 时同步直调,不经字节码帧)。
    //   - vm:宿主句柄(lua_State* 对标);报错走 vm.fail/raise(入挂起错误寄存器),分配 vm.gc()。
    //   - slots:调用区可写视图 [callee, a1..aN](argc = size()-1);slots[0] 兼**返回槽**(返回值直接写于此),方法调用形态
    //     下 VM 预先把槽 0 覆写为 receiver(仍是返回槽)。
    //   - 返回 bool:true = 成功(值已写 slots[0]);**契约 false ⟺ 已调 vm.fail/raise**,惯用法 `return vm.fail(...);`。VM
    //     调用后 drop(argc) 升 slots[0] 为栈顶,失败载荷留寄存器交调用方 unwind;消息为烘齐完整串、不含位置前缀。
    //   - 纪律:①**叶子调用**,不得操作 VM 值栈(push/pop/drop)否则 slots 失效;②写新生对象到 slots[0] 时中间对象须 Guard
    //     入临时根;③错误走侧信道寄存器,不抛 C++ 异常。
    using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);

    // 原生函数对象:把一个 C++ NativeFn 包成 aria Value。name_/fn_ 恒非空(匿名用
    // kAnonymousName);地址哈希型、final。trace 只标 name_(fn_ 是 C++ 指针非 GC 对象)。
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

        // mark_object 容 nullptr 仅防御(本应恒非空,见类注)。
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

    // 工厂:分配 ObjNativeFn。守卫纪律见 Object.hpp;调用方须自行根化 name(fn 是标量)。
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
