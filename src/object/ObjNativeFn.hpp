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

    // 原生函数的 C++ 调用签名(宿主侧实现,VM 在 CALL 命中 ObjNativeFn 时同步直接调用,不经
    // 字节码帧)。builtins(type/len/str/assert)与未来嵌入 API 皆包成此类型。
    //
    //   - vm:宿主句柄(对标 lua_State*):报错 vm.fail/vm.raise(写入当前上下文挂起错误
    //     寄存器,通道见 AriaVM.hpp)、分配 vm.gc()(跨分配持有的裸 Obj* 须 make_guard 根化;
    //     M6 协程期 vm 路由到当前协程)。
    //   - slots:调用区可写视图,指向值栈上 [callee, a1..aN] 的连续 argc+1 个槽:
    //       slots[0] = callee / **返回槽**(返回值直接写于此,省去 drop+push);
    //       slots[1..argc] = 实参;argc = slots.size() - 1。
    //     原生天然变参(不存 arity 字段),元数自查。方法调用形态(经 ObjBoundMethod 绑定):
    //     VM 调用前把槽 0 覆写为 receiver,原生收到的 slots[0] = this,同时仍是返回槽;类路径/
    //     静态访问取出裸原生值不绑定,与自由调用无异。
    //
    //   返回值 bool:true = 成功(返回值已写 slots[0]);false = 失败(已调 vm.fail/vm.raise)。
    //   **契约:return false ⟺ 已调 vm.fail/vm.raise**,惯用法一行 `return vm.fail(code, fmt,
    //   ...);` 即自动满足。VM 调用后 drop(argc),slots[0] 升栈顶即返回值;失败载荷留寄存器
    //   交调用方 unwind。消息为一步烘齐的完整烘焙串、不含位置前缀(位置由未捕获出口的跟踪
    //   行给出,原生不进帧顶帧即 caller)。
    //
    //   契约:①**叶子调用** -- 不得操作 VM 值栈(push/pop/drop),否则 slots 视图失效;只读
    //   slots[1..]、写 slots[0]、经 vm.fail/raise 报错,需回调 aria 函数属未来机制(M6 重入
    //   接缝)。②读即时值无 GC 任意阶段安全;写新生对象到 slots[0] 时中间对象须 Guard 入临时
    //   根(slots[0] 写入后即随值栈为根);vm.fail 内分配可能触 GC,载荷入寄存器后经 tracer
    //   标根。③错误走侧信道寄存器,不抛 C++ 异常、不用 longjmp。
    using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);

    // 原生函数对象:把一个 C++ NativeFn 包成 aria Value。name_ 恒非空(构造期 ASSERT,无具名
    // 需求者用 kAnonymousName);fn_ 恒非空。地址哈希型(无内容相等语义),final。trace 标
    // name_(fn_ 是 C++ 指针非 GC 对象,不标);debug_repr = `<fn name>`(与 ObjFunction 一致,
    // 用户侧不区分 native/user 函数,native 身份经 type() 反射可见)。
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

        // 调试渲染:`<fn name>`(与 ObjFunction 一致 -- 用户侧不区分 native / user 函数,
        //   native 身份经 ObjType::NATIVE_FN / type() 反射可见)。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjString* name_;
        NativeFn   fn_;
    };

    // 工厂:分配 ObjNativeFn。**调用方须在调用前自行根化 name**(intern 串是 weak root,跨
    //   new_object 顶 maybe_collect 可能被回收;工厂只做一次 new_object,不替调用方守卫);
    //   fn 是标量无需入根。name 须非空(构造期 ASSERT)。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, NativeFn fn);

    // 工厂重载(匿名):name 取 kAnonymousName("<anonymous>",`<>` 不可作标识符故具辨识度)。
    //   委托 StringView 名重载(声明于下),驻留与守卫由其内部完成。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, NativeFn fn);

    // 工厂重载(StringView 名):name 经工厂内部 intern 并自行守卫,免调用点 new_string+guard
    //   两步(bootstrap/builtins 注册等无现成 intern 串的站点)。委托上者。
    [[nodiscard]]
    ObjNativeFn* new_native_fn(GC& gc, StringView name, NativeFn fn);

} // namespace aria

#endif // ARIA_OBJ_NATIVE_FN_HPP
