#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjFunction;
    class ObjString;

    // 绑定方法对象:方法值与其绑定的接收者,在 obj.m 读取类的实例方法时产生;静态成员不产生该对象。
    class ObjBoundMethod final : public Object {
    public:
        ObjBoundMethod(ObjFunction* method, Value receiver);
        ~ObjBoundMethod() override = default;

        ObjBoundMethod(const ObjBoundMethod&)            = delete;
        ObjBoundMethod& operator=(const ObjBoundMethod&) = delete;
        ObjBoundMethod(ObjBoundMethod&&)                 = delete;
        ObjBoundMethod& operator=(ObjBoundMethod&&)      = delete;

        [[nodiscard]]
        ObjFunction* method() const noexcept {
            return method_;
        }

        [[nodiscard]]
        Value receiver() const noexcept {
            return receiver_;
        }

        // 取绑定函数的驻留名称。
        [[nodiscard]]
        ObjString* name() const noexcept;

        void trace(GC& gc) const noexcept override;

        // 壳定长(method_/receiver_ 内联在壳内,无外挂 buffer)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjBoundMethod);
        }

        // 内容相等(==):receiver 同一 && method 同一;`===` 仍按身份。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        // 调试渲染:`<bound method m>`;显示同文案。
        [[nodiscard]]
        String repr() const override;

        // 从所属类载入:自 BoundMethod 类链起查(本类无值本地状态)。
        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

    private:
        ObjFunction* method_;   // 被绑定的 Aria 或原生函数,恒非空
        Value        receiver_; // 绑定接收者,调用时写入槽 0
    };

    // 工厂:分配 ObjBoundMethod;调用方须自行根化 method 与 receiver 中的对象。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, Value receiver);

    // receiver 为对象指针的便捷重载:装箱后委托 Value 版;receiver 须非空。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, Object* receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
