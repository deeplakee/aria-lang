#include "runtime/builtins/RangeClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init(a [b]) -> range:工厂构造,1 元等价 a...(无上界)、2 元等价 a...b(不含上界),覆盖槽 0
        //(call_class 预置的临时 instance 被替换);端点须整数,文案与 MAKE_RANGE 同族。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1 && argc != 2) {
                return vm.arity_error_range(argc, 1, 2);
            }
            if (argc == 1) {
                if (!slots[1].is_int()) {
                    return vm.fail(ErrorCode::TypeMismatch, "range bound must be an integer, got {}",
                                   type_name(slots[1]));
                }
                slots[0] = Value::from_obj(new_range(vm.gc(), slots[1].as_int()));
                return true;
            }
            if (!slots[1].is_int() || !slots[2].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "range bounds must be integers, got {} and {}",
                               type_name(slots[1]), type_name(slots[2]));
            }
            slots[0] = Value::from_obj(new_range(vm.gc(), slots[1].as_int(), slots[2].as_int(), true));
            return true;
        }

        // iter() -> 迭代器。GC 约束:range 在 slots[0] 于栈根,迭代器白色建成先写回槽发布再返回,
        // 中间无 GC 点;迭代器标量自足不持源指针,源 range 彼后可回收。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto range = Object::as<ObjRange>(slots[0].as_obj());
            slots[0]         = Value::from_obj(new_range_iterator(vm.gc(), range));
            return true;
        }

        // range 方法表(has_next/next 住 Iterator 表,全子类共享)。
        constexpr BuiltinFnEntry kRangeBuiltins[] = {
                {"init", fn_init},
                {"iter", fn_iter},
        };

    } // namespace

    ObjClass* RangeClass::make_class(GC& gc, ObjClass* super) {
        // Range bootstrap 类:内置 range 的语言方法面载体,类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "Range", super);
        Builtin::register_class_methods(gc, klass, kRangeBuiltins);
        return klass;
    }

} // namespace aria
