#ifndef ARIA_BUILTIN_HPP
#define ARIA_BUILTIN_HPP

#include "common.hpp"
#include "runtime/NativeCallInfo.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class AriaHashTable;

    // 内建面目录伞:Builtin 全局内建与 <Name>Class 类型方法面;内建函数注册进 VM 级只读 builtins_ 表。

    // 内建函数表条目:名 + 原生函数指针(全局表/类方法表共此一形态)。
    struct BuiltinFnEntry {
        StringView name;
        NativeFn   fn;
    };

    // 全局内建面宿主:实现与私有表住同名 .cpp,公有面 = 类表装载底座加编排口 register_builtins
    //(AriaVM ctor 在 set_vm_roots 后调用一次)。
    class Builtin {
    public:
        // 按名把内建方法表逐条注册进类字段表,成为该类实例的内建方法面(receiver 恒绑定)。
        static void register_class_methods(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> methods);

        // 注册全部内置;由 AriaVM ctor 在 set_vm_roots 之后调用一次。
        static void register_builtins(GC& gc, AriaHashTable& builtins);
    };

} // namespace aria

#endif // ARIA_BUILTIN_HPP
