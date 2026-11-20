#ifndef ARIA_BUILTIN_HPP
#define ARIA_BUILTIN_HPP

#include "common.hpp"
#include "runtime/NativeCallInfo.hpp"

namespace aria {

    class GC;
    class ObjClass;
    class AriaHashTable;

    struct BuiltinFnEntry {
        StringView name;
        NativeFn   fn;
    };

    class Builtin {
    public:
        // 注册为实例方法，读取时绑定接收者。
        static void register_class_methods(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> methods);

        // 注册为静态函数，不设置定义类。
        static void register_class_functions(GC& gc, ObjClass* klass, Span<const BuiltinFnEntry> functions);

        // 注册普通原生函数。GC 开启时 table 的宿主须在注册期间保持可达。
        static void register_functions(GC& gc, AriaHashTable& table, Span<const BuiltinFnEntry> functions);

        // 仅在宿主已注册 tracer 且暂停 GC 的构造期调用。
        static void register_builtins(GC& gc, AriaHashTable& builtins);
    };

} // namespace aria

#endif // ARIA_BUILTIN_HPP
