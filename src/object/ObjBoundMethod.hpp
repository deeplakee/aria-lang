#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjString;

    // 绑定方法对象:方法值与其绑定的接收者,在 obj.m 读取类的实例方法时产生;静态成员不产生该对象。
    class ObjBoundMethod final : public Object {
    public:
        ObjBoundMethod(Value method, Value receiver);
        ~ObjBoundMethod() override = default; // method_/receiver_ 是 GC 值,不归本类释放

        ObjBoundMethod(const ObjBoundMethod&)            = delete;
        ObjBoundMethod& operator=(const ObjBoundMethod&) = delete;
        ObjBoundMethod(ObjBoundMethod&&)                 = delete;
        ObjBoundMethod& operator=(ObjBoundMethod&&)      = delete;

        [[nodiscard]]
        Value method() const noexcept {
            return method_;
        }

        [[nodiscard]]
        Value receiver() const noexcept {
            return receiver_;
        }

        // 绑定的方法对象之名(闭包取 fn 名、原生取 name_,intern 恒非空);非虚、定义在 .cpp。
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
        String debug_repr() const override;

    private:
        Value method_;   // 被绑定方法值(闭包/原生;ctor 断言可调用)
        Value receiver_; // 绑定接收者(Value 泛化;闭包解包作方法帧槽 0 的 this,原生覆写调用区槽 0)
    };

    // 工厂:分配 ObjBoundMethod;调用方须自行根化 method 与 receiver 中的对象,建成须立即发布进根。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, Value method, Value receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
