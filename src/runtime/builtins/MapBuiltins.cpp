#include "runtime/builtins/MapBuiltins.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjMap.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        struct MapBuiltinEntry {
            StringView name;
            NativeFn   fn;
        };

        // iter() -> 迭代器:铸造 ObjMapIterator(map 与其迭代器成对,铸造口按类型解开
        // receiver)。GC 时序同 ListBuiltins::iter_fn:map 在 slots[0] 于栈根,迭代器白色
        // 建成**先写回槽发布再返回**,中间无 GC 点;此后 map 经迭代器 trace 可达。
        bool iter_fn(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.fail(ErrorCode::WrongArity, "iter expects no arguments, got {}", argc);
            }
            const auto map = Object::as<ObjMap>(slots[0].as_obj());
            slots[0]       = Value::from_obj(new_map_iterator(vm.gc(), map));
            return true;
        }

        // map 方法表:注册进 Map bootstrap 类(kListBuiltins 同款循环)。has_next/next 不在
        // 此表 --它们住 Iterator bootstrap 类表,全子类共享(批 4 拍板,每源只加迭代器子类)。
        constexpr MapBuiltinEntry kMapBuiltins[] = {
                {"iter", iter_fn},
        };

    } // namespace

    void register_map_builtins(GC& gc, ObjClass* klass) {
        for (const auto& [name, fn]: kMapBuiltins) {
            const auto fn_obj = new_native_fn(gc, name, fn);
            klass->set_field(fn_obj->name(), Value::from_obj(fn_obj));
        }
    }

} // namespace aria
