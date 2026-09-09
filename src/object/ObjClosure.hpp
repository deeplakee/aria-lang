#ifndef ARIA_OBJ_CLOSURE_HPP
#define ARIA_OBJ_CLOSURE_HPP

#include "memory/Array.hpp"
#include "memory/GC.hpp" // Array<ObjUpvalue*> 成员以 GC 为分配器,实例化点须 GC 完整(仓库约定:具体类自 include,不经容器传递)
#include "object/Object.hpp"

namespace aria {

    class ObjFunction;
    class ObjUpvalue;
    class ObjClass;

    // 闭包对象:函数 + 捕获的 upvalue 数组(ObjType::CLOSURE,M4 闭包)。
    //
    //   - function_:被包的 ObjFunction(字节码/常量/名字都在其上,恒非空,ctor ASSERT)。
    //     callable 收敛为闭包后(M4 阶段 2 起),ObjFunction 退为常量池内部物,运行期一律以
    //     ObjClosure 进帧。
    //   - upvalues_:捕获数组,与 fn 的捕获描述表(ObjFunction::upvalue_descs())按下标一一对应。
    //     ctor 时为空,VM 执行 CLOSURE 指令时按描述表逐个后填(new_upvalue 建或沿开链复用),
    //     之后只读。
    //   - defining_class_:方法闭包所属的 defining class(M5)。MAKE_METHOD 注册时 set,其余
    //     闭包恒 nullptr(ctor 默认):LOAD_SUPER_METHOD 从 frame.closure 直读它,沿其 superclass
    //     链查被覆写前的实现。挂闭包而非 ObjFunction(共享编译期常量)的理由:M5 计划 §2.6 --
    //     函数体内 def 执行 N 次产生 N 个类共用同一 fn 常量,superclass 运行期解析可重绑,
    //     戳共享 fn 会跨实例串链;闭包每实例一份,无共享可变状态。可为 nullptr(非方法闭包),
    //     trace 容 nullptr。
    //
    //   地址哈希型可变对象(走 Object{ObjType::CLOSURE} ctor);equals 保持默认地址相等--
    //     闭包按身份判等(同一 fn 的两次捕获是不同闭包)。
    //   final,不再派生;Array 成员自身禁拷贝/禁移动(同 ObjFunction 持 CodeUnit)。
    //   trace():标 function_ + 全部 upvalue(upvalue 再各自标其槽值/闭值)+ defining_class_
    //     (容 nullptr;类静态表的方法闭包经此级联标 defining class)。
    //   to_string():委托 function_->to_string() 渲染 `<fn name>`(与纯函数同文案,
    //     trace_execution 等调试输出兼容;闭包身份经 type()/type_name() 反射可见)。
    class ObjClosure final : public Object {
    public:
        ObjClosure(GC& gc, ObjFunction* function);
        ~ObjClosure() override = default; // upvalues_ 数组自释放,壳定长

        [[nodiscard]]
        ObjFunction* function() const noexcept {
            return function_;
        }

        // 捕获数组(与 fn.upvalue_descs() 按下标对应;CLOSURE 执行期逐个后填,之后只读)。
        [[nodiscard]]
        const Array<ObjUpvalue*>& upvalues() const noexcept {
            return upvalues_;
        }

        [[nodiscard]]
        usize upvalue_count() const noexcept {
            return upvalues_.size();
        }

        // CLOSURE 执行期逐个后填用(push 走 trivial 分配不触 GC,靠 GC 核心不变式免逐个守卫)。
        void add_upvalue(ObjUpvalue* uv);

        // defining class(M5):方法闭包所属类,MAKE_METHOD 注册时 set,之后只读;
        // 非方法闭包恒 nullptr(ctor 默认)。LOAD_SUPER_METHOD 从 frame.closure 直读。
        [[nodiscard]]
        ObjClass* defining_class() const noexcept {
            return defining_class_;
        }

        void set_defining_class(ObjClass* cls) noexcept { defining_class_ = cls; }

        // 标 function_ + 全部 upvalue + defining_class_(容 nullptr)。
        // function_ 非空;mark_object 容 nullptr 仅防御。
        void trace(GC& gc) const noexcept override;

        // 壳定长(upvalues_ 内部 Array 自管理,~Array 级联释放)。
        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjClosure);
        }

        // 可读描述:委托 function_->to_string(),渲染 `<fn name>`(与 ObjFunction 同文案)。
        [[nodiscard]]
        String to_string() const override;

    private:
        ObjFunction*       function_;       // 被包函数(恒非空,ctor ASSERT)
        Array<ObjUpvalue*> upvalues_;       // 捕获数组(ctor 空,CLOSURE 执行期逐个后填)
        ObjClass*          defining_class_; // 方法闭包所属类(ctor nullptr;MAKE_METHOD 注册时 set,之后只读)
    };

    // 工厂:分配 ObjClosure(upvalues_ 空态)。工厂不替调用方守卫入参(「每方只守自己创建的」)--
    //     只做一次 new_object、无内部新建对象;function_ 通常已入常量池(根)或调用方自行守卫,
    //     返回对象白色无根,调用方须立即发布进根(压值栈/入常量池等)。
    [[nodiscard]]
    ObjClosure* new_closure(GC& gc, ObjFunction* function);

} // namespace aria

#endif // ARIA_OBJ_CLOSURE_HPP
