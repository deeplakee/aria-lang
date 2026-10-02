#ifndef ARIA_COROUTINE_MODULE_HPP
#define ARIA_COROUTINE_MODULE_HPP

#include "common.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    class AriaVM;
    class GC;
    class ObjModule;

    // <coroutine> 内建模块自持单元:模块构造与四原语方法面全收私有,唯一公有口是收尾的
    // make_module;四原语为 NativeFn 签名静态方法,resume 须访问 AriaVM 私有面故为友元。
    class CoroutineModule {
        static bool create(AriaVM& vm, Span<Value> slots);

        static bool resume(AriaVM& vm, Span<Value> slots);

        static bool yield(AriaVM& vm, Span<Value> slots);

        static bool status(AriaVM& vm, Span<Value> slots);

        // 方法表;slots = [<coroutine> 模块(接收者兼返回槽), co, payload..]。create/status 普通
        // 原生;resume/yield 切换型:返 true 时 current_ 已切至对侧且不写 slots[0](对侧预留结果槽,
        // 由 yield/RETURN 完成写;对侧死于未捕获错误时载荷走本侧挂起寄存器),false ⟺ 已 fail 未切换。
        static constexpr BuiltinFnEntry kModuleFunctions[] = {
                {"create", create},
                {"resume", resume},
                {"yield", yield},
                {"status", status},
        };

        static void register_functions(GC& gc, ObjModule* module);

    public:
        // <coroutine> 合成模块的唯一构造口:建模块、装方法面、以 Value 返回;装在 AriaVM ctor
        // 构造临界区内(入表即根,免守卫),模块永不入 modules_ 表。
        static Value make_module(GC& gc);
    };

} // namespace aria

#endif // ARIA_COROUTINE_MODULE_HPP
