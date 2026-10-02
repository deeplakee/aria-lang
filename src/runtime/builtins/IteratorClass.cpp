#include "runtime/builtins/IteratorClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // has_next() -> bool:是否还有下一个元素。
        bool fn_has_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            // 绑定路径契约:slots[0] 恒 ObjIterator 子类实例,DEBUG 下 as 走 dynamic_cast 校验。
            const auto iter = Object::as<ObjIterator>(slots[0].as_obj());
            slots[0]        = Value::from_bool(iter->has_next());
            return true;
        }

        // next() -> 下一元素并推进游标;越界抛 IterationExhausted(fail-fast,可 catch)。
        bool fn_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto iter  = Object::as<ObjIterator>(slots[0].as_obj());
            const auto value = iter->next(vm);
            if (!value) {
                return false; // 已 fail(IterationExhausted)
            }
            slots[0] = *value;
            return true;
        }

        // 迭代器方法表(注册名须与 for-in 降糖发射的同名常量同拼写)。
        constexpr BuiltinFnEntry kIteratorBuiltins[] = {
                {"has_next", fn_has_next},
                {"next", fn_next},
        };

    } // namespace

    ObjClass* IteratorClass::make_class(GC& gc, ObjClass* super) {
        // Iterator bootstrap 类:迭代器语言方法面载体,类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "Iterator", super);
        Builtin::register_class_methods(gc, klass, kIteratorBuiltins);
        return klass;
    }

} // namespace aria
