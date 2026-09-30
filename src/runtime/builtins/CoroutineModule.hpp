#ifndef ARIA_COROUTINE_MODULE_HPP
#define ARIA_COROUTINE_MODULE_HPP

#include "common.hpp"
#include "runtime/builtins/Builtin.hpp" // BuiltinFnEntry(方法表条目形态)
#include "value/Value.hpp"

namespace aria {

    class AriaVM;
    class GC;
    class ObjModule;

    // <coroutine> 内建模块的自持单元:模块构造与四原语方法面全收私有,**唯一公有口是收尾的
    // make_module** -- 语言面实现不外泄。四原语是 NativeFn 签名的静态方法、AriaVM
    // 的友元(resume 须访问 check_arity/prepare_call_args/current_ 等私有面,自由函数不可达)。
    // 静态类无实例(与 ObjModule 的模块对象概念经此消歧)。切换契约见方法表上方注释,
    // 方法体住 CoroutineModule.cpp。
    class CoroutineModule {
        static bool create(AriaVM& vm, Span<Value> slots);

        static bool resume(AriaVM& vm, Span<Value> slots);

        static bool yield(AriaVM& vm, Span<Value> slots);

        static bool status(AriaVM& vm, Span<Value> slots);

        // 方法表:装载经 register_functions,条目即上四原语。通用形 slots = [<coroutine> 模块
        //(接收者兼返回槽), co, payload..],argc = slots.size() - 1。create/status 是普通原生;
        // resume/yield 是切换型:**返 true 时 current_ 已切至对侧上下文**且不写 slots[0](该槽
        // 是对侧要写的预留结果槽,由 yield/RETURN 完成写;对侧死于未捕获错误时载荷走本侧挂起
        // 寄存器经 unwind 链式多跳,该槽随调用区一并截弃);false ⟺ 已 vm.fail 且未切换。
        static constexpr BuiltinFnEntry kModuleFunctions[] = {
                {"create", create},
                {"resume", resume},
                {"yield", yield},
                {"status", status},
        };

        // 把方法表装进 <coroutine> 模块 globals(经 Builtin::register_module_functions 底座)。
        static void register_functions(GC& gc, ObjModule* module);

    public:
        // <coroutine> 合成模块的唯一构造口:建模块、装方法面、以 Value 返回。经 Builtin.cpp
        // 的 kBuiltinVars 变量腿装载("coroutine" 键),调用点在 AriaVM ctor 构造临界区内
        // (创建免守卫,入表即根);模块永不入 modules_ 表。
        static Value make_module(GC& gc);
    };

} // namespace aria

#endif // ARIA_COROUTINE_MODULE_HPP
