#include "runtime/builtins/IteratorBuiltins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // ---- 迭代器方法实现(NativeFn 方法调用形态:slots[0] = receiver 兼返回槽) ----
        // 两原生都是 ObjIterator 引擎缝的薄壳:虚分派到各源子类(list/string/map/range),
        // 本文件不认识任何具体源。

        // has_next() -> bool:是否还有下一个元素。
        bool fn_has_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "has_next expects no arguments, got {}", argc);
            }
            // 绑定路径契约:slots[0] 恒 ObjIterator 子类实例,DEBUG 下 as 走 dynamic_cast 校验。
            const auto iter = Object::as<ObjIterator>(slots[0].as_obj());
            slots[0]        = Value::from_bool(iter->has_next());
            return true;
        }

        // next() -> 下一元素:取下一值并推进游标;越界抛 IterationExhausted(fail-fast,
        // 可 catch)。
        bool fn_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "next expects no arguments, got {}", argc);
            }
            const auto iter  = Object::as<ObjIterator>(slots[0].as_obj());
            const auto value = iter->next(vm);
            if (!value) {
                return false; // 已 fail(IterationExhausted)
            }
            slots[0] = *value;
            return true;
        }

        // 迭代器方法表:注册进 Iterator bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp;
        // 注册名与 CodeGen forIn 降糖 emit_method_call0 发射的同名常量同指针,查表按指针命中)。
        constexpr builtins::BuiltinEntry kIteratorBuiltins[] = {
                {"has_next", fn_has_next},
                {"next", fn_next},
        };

    } // namespace

    void register_iterator_builtins(GC& gc, ObjClass* klass) { register_builtin_methods(gc, klass, kIteratorBuiltins); }

} // namespace aria
