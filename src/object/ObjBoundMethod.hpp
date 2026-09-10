#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp" // Value(成员 method_/receiver_,与 ObjUpvalue 同款子类型头直 include)

namespace aria {

    class GC;

    // 绑定方法对象:被绑定的方法值 + 绑定的接收者(ObjType::BOUND_METHOD)。`obj.m` 命中类表
    // 方法时由 LOAD_FIELD 现场绑定(new_bound_method),call_value BOUND_METHOD 分支解包进帧/直调。
    //
    //   - method_:被绑定的方法值(**Value** -- 可为 ObjClosure 或 ObjNativeFn,ctor 断言可调用;
    //     M5 阶段 2 泛化:原生方法经同一绑定形态承载,内建类型方法的载体,调用约定见 ObjNativeFn
    //     契约「方法调用形态」-- slots[0] = this,同时是返回槽)。静态成员经 `Foo.m` 取出的是裸值
    //     (闭包/原生原样),不经本类型(无 this 不绑)。
    //   - receiver_:绑定的接收者(**Value** 而非 ObjInstance* -- 为将来内建类型方法留泛化,
    //     如 list.m;当前 M5 只产 ObjInstance 接收者)。闭包方法解包时作为实例方法帧槽 0 的
    //     this(方法帧 [this, a1..aN]);原生方法调用时覆写调用区槽 0(与返回槽同位)。
    //
    //   地址哈希型可变对象(走 Object{ObjType::BOUND_METHOD} ctor);equals 保持默认地址相等--
    //     绑定按身份判等(同一方法绑不同实例是不同对象,绑定语义上无内容相等需求)。final。
    //   trace():mark_value(method_) + mark_value(receiver_)(两者皆装箱任意对象,值级联)。
    //   debug_repr():`<bound method m>`(m = method_ 内部函数名:闭包 -> fn 名、原生 -> name_;
    //     与 <fn m> 渲染族一致);基类 to_string 默认委托之,显示同文案。
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

        // 方法名的非虚读取(闭包取 fn 名、原生取 name_,与 debug_repr 渲染同源):供
        // debug_repr 渲染共用。定义在 .cpp(需 ObjClosure/ObjNativeFn 完整类型)。
        [[nodiscard]]
        StringView method_name() const noexcept;

        // 标 method_ + receiver_ 值。
        void trace(GC& gc) const noexcept override;

        // 壳定长(method_/receiver_ 内联在壳内,无外挂 buffer)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjBoundMethod);
        }

        // 调试渲染:`<bound method m>`;基类 to_string 默认委托本方法,显示同文案。
        [[nodiscard]]
        String debug_repr() const override;

    private:
        Value method_;   // 被绑定方法值(闭包/原生;ctor 断言可调用)
        Value receiver_; // 绑定接收者(Value 泛化;闭包解包作方法帧槽 0 的 this,原生覆写调用区槽 0)
    };

    // 工厂:分配 ObjBoundMethod。工厂不替调用方守卫入参(「每方只守自己创建的」)--只做一次
    //     new_object、无内部新建对象,调用方须在调用前自行根化 method 与 receiver 中的对象
    //     (LOAD_FIELD 路径:instance 仍 peek 在栈根化,方法值在类表内可达)。返回对象白色无根,
    //     须立即发布进根(LOAD_FIELD 先写回原槽根化再回填缓存)。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, Value method, Value receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
