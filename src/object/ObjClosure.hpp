#ifndef ARIA_OBJ_CLOSURE_HPP
#define ARIA_OBJ_CLOSURE_HPP

#include "memory/Array.hpp"
#include "memory/GC.hpp"
#include "object/Object.hpp"

namespace aria {

    class ObjFunction;
    class ObjUpvalue;
    class ObjClass;
    class ObjString;

    // 闭包对象:函数 + 捕获的 upvalue 数组。defining_class_ 非空时表示这是从实例方法注册出来的闭包,
    // 它同时充当方法性标记与 super 解析来源。
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

        [[nodiscard]]
        bool is_method() const noexcept {
            return defining_class_ != nullptr;
        }

        // CLOSURE 执行期逐个后填用(push 走 trivial 分配不触 GC,免逐个守卫)。
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

    // 工厂:分配 ObjClosure(upvalues_ 空态);function 通常已入常量池(根),无需另守。
    [[nodiscard]]
    ObjClosure* new_closure(GC& gc, ObjFunction* function);

} // namespace aria

#endif // ARIA_OBJ_CLOSURE_HPP
