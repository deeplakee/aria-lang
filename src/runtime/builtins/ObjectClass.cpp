#include "runtime/builtins/ObjectClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // Object 根类方法实现(NativeFn 方法调用形态见 Builtins.hpp)。

        // init():no-op -- 返回 true 不写槽,槽 0 原样即返回实例(不合成 ObjFunction,保
        // 「module 恒非空」不变式);未写 init 的类沿链继承本实现。
        bool fn_init(AriaVM&, Span<Value>) { return true; }

        // Object 根类方法表:注册进 Object bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        constexpr builtins::BuiltinEntry kObjectBuiltins[] = {
                {"init", fn_init},
        };

    } // namespace

    void ObjectClass::register_methods(GC& gc, ObjClass* klass) { register_class_methods(gc, klass, kObjectBuiltins); }

} // namespace aria
