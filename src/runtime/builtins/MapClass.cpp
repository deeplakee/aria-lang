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

        // map 方法实现(NativeFn 方法调用形态见 Builtin.hpp)惯例:receiver 解开后直取 table()
        // 绑为 table(仅 iter 需要 ObjMap* 本体传给迭代器)。键判定一律走表内判等 ===(find 即
        // value_identical 匹配,int 1 与 f64 1.0 是不同键),与下标读同域 -- 判键的方法不做
        // value_equal 内容相等,那是 list 的域。

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

        // has(key) -> Bool:键存在判定(miss 返 false 不报错,与 get 同族)。方法名取 has 不取
        // contains --map 上 contains 有「判键还是判值」二义。
        bool fn_has(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]          = Value::from_bool(table.find(slots[1]) != nullptr);
            return true;
        }

        // get(key) -> 值或 nil:不带 KeyError 的读(miss 与「键存在而值为 nil」在返回值上不可分,
        // 需要分清时用 has;下标读 m[k] miss 仍报 KeyError)。
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

        // remove(key) -> Bool:移除命中键(置墓碑,无分配),命中 true、miss false 不报错 --与 list
        // remove 同口径。
        bool fn_remove(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            auto& table = Object::as<ObjMap>(slots[0].as_obj())->table();
            slots[0]    = Value::from_bool(table.erase(slots[1]));
            return true;
        }

        // clear() -> nil:清空(对数归零,容量保留)。重绑 m = {} 换新表别名仍见旧内容,本方法供
        // 共享可变状态原地清空(与 list clear 同款)。
        bool fn_clear(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            Object::as<ObjMap>(slots[0].as_obj())->table().clear();
            slots[0] = Value::nil_val();
            return true;
        }

        // keys() -> list:全部键的快照(新 list,与源 map 解耦;序随占用槽,unspecified)。GC 约束:
        // map 在 slots[0] 于栈根,new_list 顶部 maybe_collect 时新 list 未诞生,逐键 push 走 trivial
        // 分配不触 GC,建成随返回值写回槽发布,窗口内无 GC 点。
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

        // values() -> list:全部值的快照(同 keys 的分配与 GC 时序)。与 keys 同槽位序推进,故两次
        // 调用产出的 keys()[i] 与 values()[i] 同源同对(序本身仍 unspecified,不可跨调用依赖)。
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

        // pairs() -> list:全部键值对的快照(新 list,与源 map 解耦)。每元素是 [k, v] 二元 list,
        // 与 keys()/values() 同槽位序,故 pairs()[i] == [keys()[i], values()[i]]。GC 约束:本表唯一
        // 「元素本身也是新对象」的方法 -- 循环里每铸一个内层 list 都过 new_list 顶 maybe_collect,
        // 而 receiver 之外无根的是外层与已铸内层,故外层须挂守卫(内层铸后仅 push 即入外层,窗口内
        // 无 GC 点,无需各自挂)。
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

        // iter() -> 迭代器:map 与其迭代器成对(铸造口按类型解开 receiver)。GC 约束:map 在
        // slots[0] 于栈根,迭代器白色建成**先写回槽发布再返回**,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto map = Object::as<ObjMap>(slots[0].as_obj());
            slots[0]       = Value::from_obj(new_map_iterator(vm.gc(), map));
            return true;
        }

        // map 方法表:注册进 Map bootstrap 类(注册机制见 runtime/builtins/Builtin.hpp)。
        // has_next/next 不在此表 --它们住 Iterator bootstrap 类表,全子类共享(每源只加迭代器子类)。
        constexpr BuiltinFnEntry kMapBuiltins[] = {
                {"size", fn_size},   {"is_empty", fn_is_empty}, {"has", fn_has},     {"get", fn_get},
                {"keys", fn_keys},   {"values", fn_values},     {"pairs", fn_pairs}, {"remove", fn_remove},
                {"clear", fn_clear}, {"iter", fn_iter},
        };

    } // namespace

    ObjClass* MapClass::make_class(GC& gc, ObjClass* super) {
        // Map bootstrap 类:内置 map 的语言方法面载体,经 ObjMap::load_field_bound 查表命中后恒绑定
        // 触达(曝光契约见 runtime/value_register.hpp 表头)。类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "Map", super);
        Builtin::register_class_methods(gc, klass, kMapBuiltins);
        return klass;
    }

} // namespace aria
