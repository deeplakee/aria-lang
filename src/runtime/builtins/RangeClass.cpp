#include "runtime/builtins/RangeClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtins.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // iter() -> 迭代器:range 与其迭代器成对(铸造口按类型解开 receiver)。GC 约束:range 在
        // slots[0] 于栈根,迭代器白色建成**先写回槽发布再返回**,中间无 GC 点;此后源 range
        // 不可达可回收,迭代器标量自足(不持源指针,见 ObjRangeIterator 头注释)。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto range = Object::as<ObjRange>(slots[0].as_obj());
            slots[0]         = Value::from_obj(new_range_iterator(vm.gc(), range));
            return true;
        }

        // range 方法表:注册进 Range bootstrap 类(注册机制见 runtime/builtins/Builtins.hpp)。
        // has_next/next 不在此表 --它们住 Iterator bootstrap 类表,全子类共享(每源只加迭代器子类)。
        constexpr builtins::BuiltinFnEntry kRangeBuiltins[] = {
                {"iter", fn_iter},
        };

    } // namespace

    void RangeClass::register_methods(GC& gc, ObjClass* klass) { register_class_methods(gc, klass, kRangeBuiltins); }

} // namespace aria
