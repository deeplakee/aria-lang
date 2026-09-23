#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp" // Value(成员 method_/receiver_,与 ObjUpvalue 同款子类型头直 include)

namespace aria {

    class GC;
    class ObjString;

    // 绑定方法对象:被绑定的方法值 + 绑定的接收者(ObjType::BOUND_METHOD)。`obj.m` 命中类表
    // 方法时由 LOAD_FIELD 现场绑定(new_bound_method),call_value BOUND_METHOD 分支解包进帧/直调。
    //
    //   - method_:被绑定的方法值(**Value** -- 闭包或原生函数,ctor 断言可调用;原生方法经同一
    //     绑定形态承载,调用约定见 ObjNativeFn 契约「方法调用形态」-- slots[0] = this,同时是
    //     返回槽)。静态成员经 `Foo.m` 取出的是裸值(闭包/原生原样),不经本类型(无 this 不绑)。
    //   - receiver_:绑定的接收者(**Value** 而非 ObjInstance* -- 为内建类型方法留泛化,如
    //     list.m;当前只产 ObjInstance 接收者)。闭包方法解包时作为实例方法帧槽 0 的
    //     this(方法帧 [this, a1..aN]);原生方法调用时覆写调用区槽 0(与返回槽同位)。
    //
    //   地址哈希型可变对象(走 Object{ObjType::BOUND_METHOD} ctor);equals 按**内容相等**:
    //     receiver 同一 && method 同一(Python bound method 的 `__eq__` 同款)。bound 缓存取消后
    //     读路径每次访问都是新对象,`obj.m == obj.m` 靠本判等为真,而 `===`(身份)为假 -- 恰与
    //     语言既有的 `==`(value_equal)/`===`(identity) 二分一致。final。
    //   trace():mark_value(method_) + mark_value(receiver_)。
    //   debug_repr():`<bound method m>`(m 经非虚 name() 取名);基类 to_string 默认
    //     委托之,显示同文案。
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

        // 名字访问器:绑定的方法对象之名(闭包取 fn 名、原生取 name_,intern 驻留恒非空),
        // 与 ObjFunction/ObjClosure 等的 name() 同约定;非虚、定义在 .cpp(需 ObjClosure/
        // ObjNativeFn 完整类型),与 debug_repr 渲染同源。
        [[nodiscard]]
        ObjString* name() const noexcept;

        // 内容相等(==):receiver 同一 && method 同一;`===` 仍按身份(每次访问的 bound 是新对象)。
        [[nodiscard]]
        bool equals(const Object* other) const noexcept override;

        void trace(GC& gc) const noexcept override;

        // 壳定长(method_/receiver_ 内联在壳内,无外挂 buffer)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjBoundMethod);
        }

        // 调试渲染:`<bound method m>`;显示同文案(to_string 经基类默认委托)。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        Value method_;   // 被绑定方法值(闭包/原生;ctor 断言可调用)
        Value receiver_; // 绑定接收者(Value 泛化;闭包解包作方法帧槽 0 的 this,原生覆写调用区槽 0)
    };

    // 工厂:分配 ObjBoundMethod。工厂不替调用方守卫入参(「每方只守自己创建的」)--只做一次
    //     new_object、无内部新建对象,调用方须在调用前自行根化 method 与 receiver 中的对象
    //     (LOAD_FIELD 路径:instance 仍 peek 在栈根化,方法值在类表内可达)。返回对象白色无根,
    //     须立即发布进根(LOAD_FIELD 先写回原槽根化)。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, Value method, Value receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
