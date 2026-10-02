#include "runtime/builtins/IteratorClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjIterator.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "object/iterator/ObjStringIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init(src) -> 迭代器:工厂构造,按源类型铸对应迭代器覆盖槽 0(call_class 预置的临时
        // instance 被替换);只收 string/list/map,源在槽 1 即根,白色迭代器写槽即发布。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (const auto list = try_obj<ObjList>(slots[1])) {
                slots[0] = Value::from_obj(new_list_iterator(vm.gc(), list));
                return true;
            }
            if (const auto str = try_obj<ObjString>(slots[1])) {
                slots[0] = Value::from_obj(new_string_iterator(vm.gc(), str));
                return true;
            }
            if (const auto map = try_obj<ObjMap>(slots[1])) {
                slots[0] = Value::from_obj(new_map_iterator(vm.gc(), map));
                return true;
            }
            return vm.fail(ErrorCode::TypeMismatch, "iterator source must be a string, list or map, got {}",
                           type_name(slots[1]));
        }

        // has_next() -> bool:是否还有下一个元素。
        bool fn_has_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjIterator>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->has_next());
            return true;
        }

        // next() -> 下一元素并推进游标;越界抛 IterationExhausted(fail-fast,可 catch)。
        bool fn_next(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjIterator>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto value = self->next(vm);
            if (!value) {
                return false; // 已 fail(IterationExhausted)
            }
            slots[0] = *value;
            return true;
        }

        // 迭代器方法表(注册名须与 for-in 降糖发射的同名常量同拼写)。
        constexpr BuiltinFnEntry kIteratorBuiltins[] = {
                {"init", fn_init},
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
