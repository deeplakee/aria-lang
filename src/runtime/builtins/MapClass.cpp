#include "runtime/builtins/MapClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 键判定一律走表内判等 ===(value_identical 匹配,int 1 与 f64 1.0 是不同键),与下标读同域。

        // init() -> map:工厂构造,空表覆盖槽 0(call_class 预置的临时 instance 被替换);只收 0 元。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            slots[0] = Value::from_obj(new_map(vm.gc()));
            return true;
        }

        // size() -> 整数:键值对数。
        bool fn_size(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]          = Value::from_int(static_cast<i64>(table.size()));
            return true;
        }

        // is_empty() -> Bool:无键值对判定(与 list 同名的空表谓词)。
        bool fn_is_empty(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]          = Value::from_bool(table.empty());
            return true;
        }

        // has(key) -> Bool:键存在判定(miss 返 false 不报错)。
        bool fn_has(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]          = Value::from_bool(table.find(slots[1]) != nullptr);
            return true;
        }

        // get(key) -> 值或 nil:miss 取 nil(与「值为 nil」不可分,分清用 has);下标读 miss 仍报 KeyError。
        bool fn_get(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            const auto  entry = table.find(slots[1]);
            slots[0]          = entry != nullptr ? entry->value : Value::nil_val();
            return true;
        }

        // remove(key) -> Bool:移除命中键(命中 true、miss false 不报错)。
        bool fn_remove(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]    = Value::from_bool(table.erase(slots[1]));
            return true;
        }

        // clear() -> nil:清空(对数归零,容量保留)。
        bool fn_clear(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            Object::as<ObjMap>(slots[0].as_obj())->table().clear();
            slots[0] = Value::nil_val();
            return true;
        }

        // keys() -> list:全部键的快照(序随占用槽,unspecified)。GC 约束:map 在 slots[0] 于栈根,
        // new_list 顶部 maybe_collect 时新 list 未诞生,逐键 push trivial 不触 GC,建成写回槽发布。
        bool fn_keys(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            const auto  list  = new_list(vm.gc());
            for (const auto& [key, _]: table) {
                list->elements().push(key);
            }
            slots[0] = Value::from_obj(list);
            return true;
        }

        // values() -> list:全部值的快照;与 keys 同槽位序,keys()[i]/values()[i] 同源同对(序 unspecified)。
        bool fn_values(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            const auto  list  = new_list(vm.gc());
            for (const auto& [_, value]: table) {
                list->elements().push(value);
            }
            slots[0] = Value::from_obj(list);
            return true;
        }

        // pairs() -> list:全部 [k, v] 对的快照,与 keys()/values() 同槽位序(pairs()[i] 对齐)。
        // GC 约束:本表唯一「元素本身也是新对象」的方法 -- 外层必挂守卫(循环内每铸内层 list 都
        // 过 maybe_collect,而 receiver 之外无根);内层铸后仅 push 即入外层,窗口内无 GC 点。
        bool fn_pairs(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            const auto  pairs = new_list(vm.gc());
            const auto  guard = vm.gc().make_guard(pairs);
            for (const auto& [key, value]: table) {
                const auto pair = new_list(vm.gc());
                pair->elements().push(key);
                pair->elements().push(value);
                pairs->elements().push(Value::from_obj(pair));
            }
            slots[0] = Value::from_obj(pairs);
            return true;
        }

        // iter() -> 迭代器。GC 约束:map 在 slots[0] 于栈根,迭代器白色建成先写回槽发布再返回,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto map = Object::as<ObjMap>(slots[0].as_obj());
            slots[0]       = Value::from_obj(new_map_iterator(vm.gc(), map));
            return true;
        }

        // map 方法表(has_next/next 住 Iterator 表,全子类共享)。
        constexpr BuiltinFnEntry kMapBuiltins[] = {
                {"init", fn_init},     {"size", fn_size},   {"is_empty", fn_is_empty}, {"has", fn_has},
                {"get", fn_get},       {"keys", fn_keys},   {"values", fn_values},     {"pairs", fn_pairs},
                {"remove", fn_remove}, {"clear", fn_clear}, {"iter", fn_iter},
        };

    } // namespace

    ObjClass* MapClass::make_class(GC& gc, ObjClass* super) {
        // Map bootstrap 类:内置 map 的语言方法面载体,类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "Map", super);
        Builtin::register_class_methods(gc, klass, kMapBuiltins);
        return klass;
    }

} // namespace aria
