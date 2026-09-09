#ifndef ARIA_OBJ_BOUND_METHOD_HPP
#define ARIA_OBJ_BOUND_METHOD_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp" // Value(成员 receiver_,与 ObjUpvalue 同款子类型头直 include)

namespace aria {

    class GC;
    class ObjClosure;

    // 绑定方法对象:方法闭包 + 绑定的接收者(ObjType::BOUND_METHOD)。`obj.m` 命中类表方法时
    // 由 LOAD_FIELD 现场绑定(new_bound_method),call_value BOUND_METHOD 分支解包进帧。
    //
    //   - method_:被绑定的方法闭包(**恒非空**,ctor ASSERT)。静态方法经 `Foo.m` 取出的是裸
    //     闭包,不经本类型(无 this 不绑)。
    //   - receiver_:绑定的接收者(**Value** 而非 ObjInstance* -- 为将来内建类型方法留泛化,
    //     如 list.m;当前 M5 只产 ObjInstance 接收者)。call_value 解包时作为实例方法帧槽 1
    //     的 this。
    //
    //   地址哈希型可变对象(走 Object{ObjType::BOUND_METHOD} ctor);equals 保持默认地址相等--
    //     绑定按身份判等(同一方法绑不同实例是不同对象,绑定语义上无内容相等需求)。final。
    //   trace():标 method_ + mark_value(receiver_)(receiver 可能装箱任意对象,值级联)。
    //   to_string():`<bound method m>`(m = method_ 的函数名;与 <fn m> 渲染族一致)。
    class ObjBoundMethod final : public Object {
    public:
        ObjBoundMethod(ObjClosure* method, Value receiver);
        ~ObjBoundMethod() override = default; // method_/receiver_ 是 GC 对象/值,不归本类释放

        ObjBoundMethod(const ObjBoundMethod&)            = delete;
        ObjBoundMethod& operator=(const ObjBoundMethod&) = delete;
        ObjBoundMethod(ObjBoundMethod&&)                 = delete;
        ObjBoundMethod& operator=(ObjBoundMethod&&)      = delete;

        [[nodiscard]]
        ObjClosure* method() const noexcept {
            return method_;
        }

        // 绑定的接收者(Value:当前恒为 ObjInstance 实例,泛化留内建类型)。
        [[nodiscard]]
        Value receiver() const noexcept {
            return receiver_;
        }

        // 标 method_ + receiver_ 值。
        void trace(GC& gc) const noexcept override;

        // 壳定长(receiver_ 内联在壳内,无外挂 buffer)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjBoundMethod);
        }

        // 可读描述:`<bound method m>`。
        [[nodiscard]]
        String to_string() const override;

    private:
        ObjClosure* method_;   // 被绑定方法闭包(恒非空,ctor ASSERT)
        Value       receiver_; // 绑定接收者(Value 泛化;call_value 解包作帧槽 1 的 this)
    };

    // 工厂:分配 ObjBoundMethod。工厂不替调用方守卫入参(「每方只守自己创建的」)--只做一次
    //     new_object、无内部新建对象,调用方须在调用前自行根化 method 与 receiver 中的对象
    //     (LOAD_FIELD 路径:instance 仍 peek 在栈根化,closure 在类表内可达)。返回对象白色无根,
    //     须立即发布进根(LOAD_FIELD 先写回原槽根化再回填缓存)。
    [[nodiscard]]
    ObjBoundMethod* new_bound_method(GC& gc, ObjClosure* method, Value receiver);

} // namespace aria

#endif // ARIA_OBJ_BOUND_METHOD_HPP
