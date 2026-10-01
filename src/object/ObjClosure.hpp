#ifndef ARIA_OBJ_CLOSURE_HPP
#define ARIA_OBJ_CLOSURE_HPP

#include "memory/Array.hpp"
// GC 分配器 include 理由同 ObjFunction.hpp 注。
#include "memory/GC.hpp"
#include "object/Object.hpp"

namespace aria {

    class ObjFunction;
    class ObjUpvalue;
    class ObjClass;
    class ObjString;

    // 闭包对象:函数 + 捕获的 upvalue 数组(ObjType::CLOSURE)。
    //   - function_:被包 ObjFunction(恒非空,ctor ASSERT);运行期一律以 ObjClosure 进帧。
    //   - upvalues_:与 fn 的捕获描述表(ObjFunction::upvalue_descs())按下标一一对应;ctor 空,VM 执行 CLOSURE 时按描述表
    //     逐个后填(new_upvalue 建或沿开链复用),之后只读。
    //   - defining_class_:MAKE_METHOD 注册时 set,其余恒 nullptr(ctor 默认),一职双任:super 来源(LOAD_SUPER_FIELD 直读后
    //     沿 superclass 链查被覆写前的实现)+ **方法性标记**(读路径据非空判绑 this;MAKE_STATIC/类上赋值不戳 ⟹ 静态槽原
    //     值直读)。挂闭包而非 ObjFunction:fn 是共享编译期常量,戳共享 fn 会跨实例串链,闭包每实例一份无共享可变状态。地
    //     址哈希型(闭包按身份判等)、final。trace 标 function_ + 全部 upvalue + defining_class_。
    class ObjClosure final : public Object {
    public:
        ObjClosure(GC& gc, ObjFunction* function);
        ~ObjClosure() override = default; // upvalues_ 数组自释放,壳定长

        [[nodiscard]]
        ObjFunction* function() const noexcept {
            return function_;
        }

        // 闭包的名 = 被包函数的名(intern 恒非空);定义在 .cpp(需 ObjFunction 完整类型)。
        [[nodiscard]]
        ObjString* name() const noexcept;

        // 捕获数组(与 fn.upvalue_descs() 按下标对应;CLOSURE 执行期逐个后填,之后只读)。
        [[nodiscard]]
        const Array<ObjUpvalue*>& upvalues() const noexcept {
            return upvalues_;
        }

        // defining class(MAKE_METHOD 注册时 set,之后只读)。
        [[nodiscard]]
        ObjClass* defining_class() const noexcept {
            return defining_class_;
        }

        // 方法性标记:defining class 非空 ⟺ 方法闭包。
        [[nodiscard]]
        bool is_method() const noexcept {
            return defining_class_ != nullptr;
        }

        // CLOSURE 执行期逐个后填用(push 走 trivial 分配不触 GC,靠 GC 核心不变式免逐个守卫)。
        void add_upvalue(ObjUpvalue* uv);

        void set_defining_class(ObjClass* klass) noexcept { defining_class_ = klass; }

        // 标 function_ + 全部 upvalue + defining_class_(容 nullptr)。
        void trace(GC& gc) const noexcept override;

        // 壳定长(upvalues_ 内部 Array 自管理,~Array 级联释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjClosure);
        }

        // 调试渲染:直取 function_ 名渲染 `<fn name>`。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        ObjFunction*       function_;       // 被包函数(恒非空,ctor ASSERT)
        Array<ObjUpvalue*> upvalues_;       // 捕获数组(ctor 空,CLOSURE 执行期逐个后填)
        ObjClass*          defining_class_; // 方法闭包所属类(ctor nullptr)
    };

    // 工厂:分配 ObjClosure(upvalues_ 空态)。守卫纪律见 Object.hpp;function_ 通常已入常量池(根)。
    [[nodiscard]]
    ObjClosure* new_closure(GC& gc, ObjFunction* function);

} // namespace aria

#endif // ARIA_OBJ_CLOSURE_HPP
