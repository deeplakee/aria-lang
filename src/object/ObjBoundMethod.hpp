#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp" // Value(成员 method_/receiver_,与 ObjUpvalue 同款子类型头直 include)

namespace aria {

    class GC;
    class ObjString;

    // 绑定方法对象:方法值 + 绑定的接收者(ObjType::BOUND_METHOD)。`obj.m` 命中类表方法时由 LOAD_FIELD 现场绑定,
    // call_value 的 BOUND_METHOD 分支解包进帧/直调。
    //   - method_:Value(闭包或原生,ctor 断言可调用;原生方法调用约定见 ObjNativeFn 契约)。静态成员经 `Foo.m` 取裸值,不
    //     经本类型(无 this 不绑)。
    //   - receiver_:Value 而非 ObjInstance*(为内建类型方法留泛化;当前只产 ObjInstance 接收者)。闭包方法解包作方法帧槽
    //     0 的 this,原生方法调用时覆写调用区槽 0(与返回槽同位)。地址哈希型、final;equals 按**内容相等**(receiver 同一
    //     && method 同一):读路径每次产出新 bound,`obj.m == obj.m` 为真、`===` 为假(与 `==`(value_equal)/
    //     `===`(identity) 二分一致)。
    class ObjBoundMethod final : public Object {
    public:
        ObjBoundMethod(Value method, Value receiver);
        ~ObjBoundMethod() override = default; // method_/receiver_ 是 GC 值,不归本类释放

        ObjBoundMethod(const ObjBoundMethod&)            = delete;
        ObjBoundMethod& operator=(const ObjBoundMethod&) = delete;
        ObjBoundMethod(ObjBoundMethod&&)                 = delete;
        ObjBoundMethod& operator=(ObjBoundMethod&&)      = delete;

        // 被绑定的方法值(闭包或原生;ctor 断言可调用)。
        [[nodiscard]]
        Value method() const noexcept {
            return method_;
        }

        // 绑定的接收者(Value:当前恒为 ObjInstance 实例,泛化留内建类型)。
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

    // 工厂:分配 ObjBoundMethod(守卫纪律见 Object.hpp);调用方须自行根化 method 与 receiver 中的
    //     对象。建成须立即发布进根(LOAD_FIELD 先写回原槽根化)。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, Value method, Value receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
