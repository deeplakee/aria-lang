#ifndef ARIA_BUILTIN_HPP
#define ARIA_BUILTIN_HPP

#include "common.hpp"
#include "object/ObjNativeFn.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class ObjModule;
    class AriaHashTable;

    // 内建面目录伞:Builtin 全局内建、<Name>Class 类型方法面、<Name>Module 内建模块。内置函数与变量注册进 VM 级只读
    // builtins_ 表,经 LOAD_GLOBAL 在模块 globals 优先、miss 回退 builtins 命中;STORE_GLOBAL 只写 globals 不回退。

    // 内建函数表条目:名 + 原生函数指针(全局表/类方法表/模块方法表共此一形态)。
    struct BuiltinFnEntry {
        StringView name;
        NativeFn   fn;
    };

    // 内建变量条目的初始化函数:收 GC 引用自含构造变量值(复合 bootstrap 同在此能力圈内)。
    using VarInitFn = Value (*)(GC&);

    // 内建变量表条目:名 + 初始化函数指针(仅全局内建变量表用此形态)。
    struct BuiltinVarEntry {
        StringView name;
        VarInitFn  fn;
    };

    // 全局内建面宿主:六原生与两张私有表住同名 .cpp,公有面 = 类表/模块表两装载底座加编排口
    // register_builtins(AriaVM ctor 在 set_vm_roots 后调用一次)。
    class Builtin {
    public:
        // 按名把内建方法表逐条注册进类字段表,成为该类实例的内建方法面(receiver 恒绑定)。
        static void register_class_methods(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> methods);

        // 按名把内建方法表逐条注册进模块 globals(内建模块的成员 = 模块全局绑定);不绑定
        // receiver -- 方法调用区槽 0 恒模块值,原语不读它。
        static void register_module_functions(GC& gc, ObjModule* module, Span<const BuiltinFnEntry> fns);

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次。
        static void register_builtins(GC& gc, AriaHashTable& builtins);
    };

} // namespace aria

#endif // ARIA_BUILTIN_HPP
