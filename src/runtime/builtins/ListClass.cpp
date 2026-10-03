#include "runtime/builtins/ListClass.hpp"

#include <algorithm>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // init(...) -> list:工厂构造,实参收进新 list 覆盖槽 0(call_class 预置的临时 instance 被替换)。
        // GC 走查:实参全程在槽即根,copy_from 走 trivial 分配不触 GC,白色 list 写槽即发布。
        bool fn_init(AriaVM& vm, Span<Value> slots) {
            const auto list = new_list(vm.gc());
            list->elements().copy_from({slots.data() + 1, slots.size() - 1});
            slots[0] = Value::from_obj(list);
            return true;
        }

        // push(x) -> nil:追加 x 到末尾(任意 Value);返回 nil。
        bool fn_push(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            self->elements().push(slots[1]); // trivial 分配不触 GC
            slots[0] = Value::nil_val();
            return true;
        }

        // pop() -> 末元素;空表报 IndexOutOfBounds(fail-fast,nil 哨兵不可行 -- 元素可合法存 nil)。
        bool fn_pop(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            auto& list = self->elements();
            if (list.empty()) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "pop from empty list");
            }
            slots[0] = list[list.size() - 1];
            list.pop(); // 搬移时 receiver 已离 slots[0],trivial 无 GC 点
            return true;
        }

        // insert(i, x) -> nil:i 前插入;负数从尾计数指「该下标元素之前」,合法域 [-(size), size]
        //(上界放宽到追加位)。
        bool fn_insert(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 2) {
                return vm.arity_error(argc, 2);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "insert index must be an integer, got {}", type_name(slots[1]));
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto position = util::resolve_position(slots[1].as_int(), self->elements().size());
            if (!position) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "insert index {} out of range", slots[1].as_int());
            }
            self->elements().insert(*position, slots[2]);
            slots[0] = Value::nil_val();
            return true;
        }

        // remove(x) -> Bool:移除全部 == 命中元素;miss 返 false 不报错。
        bool fn_remove(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->elements().remove(slots[1]));
            return true;
        }

        // remove_at(i) -> i 处元素:负数从尾计数,越界报原始键值。
        bool fn_remove_at(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "remove_at index must be an integer, got {}",
                               type_name(slots[1]));
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            auto&      list = self->elements();
            const auto slot = util::resolve_index(slots[1].as_int(), list.size());
            if (!slot) {
                return vm.fail(ErrorCode::IndexOutOfBounds, "remove_at index {} out of range", slots[1].as_int());
            }
            slots[0] = list[*slot];
            list.remove_at(*slot);
            return true;
        }

        // clear() -> nil:清空(长度归零,容量保留)。
        bool fn_clear(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            self->elements().clear();
            slots[0] = Value::nil_val();
            return true;
        }

        // 排序域:可比较域两支(数值 / 字符串)加域外单列(NotComparable 覆盖 nil/bool/容器等)。
        enum class SortDomain : u8 {
            Number,
            String,
            NotComparable,
        };

        SortDomain sort_domain(const Value value) noexcept {
            if (is_num(value)) {
                return SortDomain::Number;
            }
            if (try_as_obj<ObjString>(value) != nullptr) {
                return SortDomain::String;
            }
            return SortDomain::NotComparable;
        }

        // sort() -> nil:就地升序。域 = 全数值或全字符串,先整体域检再排序,排序中途无失败路径;
        // stable_sort 临时缓冲走 std 内存,receiver 在 slots[0];稳定序:等值元素(如 int 1 与
        // f64 1.0)保输入相对序。
        bool fn_sort(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            auto& list = self->elements();
            if (!list.empty()) {
                // 首元素定域,两类皆非即报(只含其类型);其余元素逐个须同域,破域(含两类皆非)
                // 报首元素与破类元素两类类型。
                const auto domain = sort_domain(list[0]);
                if (domain == SortDomain::NotComparable) {
                    return vm.fail(ErrorCode::TypeMismatch, "sort requires all numbers or all strings, got {}",
                                   type_name(list[0]));
                }
                for (usize index = 1; index < list.size(); ++index) {
                    if (sort_domain(list[index]) != domain) {
                        return vm.fail(ErrorCode::TypeMismatch,
                                       "sort requires all numbers or all strings, got {} and {}", type_name(list[0]),
                                       type_name(list[index]));
                    }
                }
            }
            std::ranges::stable_sort(list, value_less);
            slots[0] = Value::nil_val();
            return true;
        }

        // reverse() -> nil:就地整段反转。
        bool fn_reverse(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            std::ranges::reverse(self->elements()); // trivial 交换,无 GC 点
            slots[0] = Value::nil_val();
            return true;
        }

        // find(x) -> 整数或 nil:首个 == 元素的下标,未命中 nil。
        bool fn_find(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto hit = self->elements().find(slots[1]);
            slots[0]       = hit ? Value::from_int(static_cast<i64>(*hit)) : Value::nil_val();
            return true;
        }

        // contains(x) -> Bool:成员判定(list 无 in 算子,此即成员测试口)。
        bool fn_contains(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->elements().contains(slots[1]));
            return true;
        }

        // size() -> 整数:元素数。
        bool fn_size(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_int(static_cast<i64>(self->elements().size()));
            return true;
        }

        // is_empty() -> Bool:空表判定(len == 0 的谓词形)。
        bool fn_is_empty(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_bool(self->elements().empty());
            return true;
        }

        // join(sep) -> string:元素经 format_value 转串后以 sep 连接(空 list 返空串,sep 可为空串)。
        // GC 走查:format_value 无 GC 分配,唯一分配点 new_string 时 receiver/sep 均在槽 0/1。
        bool fn_join(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto sep = try_as_obj<ObjString>(slots[1]);
            if (sep == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "join separator must be a string, got {}", type_name(slots[1]));
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto str = util::join(self->elements(), sep->view(), format_value);
            slots[0]       = Value::from_obj(new_string(vm.gc(), str));
            return true;
        }

        // iter() -> 迭代器。GC 约束:list 在 slots[0] 于栈根,迭代器白色建成先写回槽发布再返回,中间无 GC 点。
        bool fn_iter(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 0) {
                return vm.arity_error(argc, 0);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            slots[0] = Value::from_obj(new_list_iterator(vm.gc(), self));
            return true;
        }

        // 运算符重载方法:list 只重载 `+` 与 `*`。

        // __add__ -> 新 list:两表拼接;浅拷(嵌套容器共享同一对象)。
        // GC 走查:唯一分配点 new_list,两侧实参经调用区槽在栈,reserve/copy_from trivial,建成即写回槽 0。
        bool fn___add__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjList>(slots[1]);
            if (rhs == nullptr) {
                return vm.fail(ErrorCode::TypeMismatch, "__add__ requires two lists, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const auto lhs = receiver<ObjList>(vm, slots[0]);
            if (lhs == nullptr) {
                return false;
            }
            const auto out = new_list(vm.gc());
            out->elements().reserve(lhs->elements().size() + rhs->elements().size());
            out->elements().copy_from(lhs->elements());
            out->elements().copy_from(rhs->elements());
            slots[0] = Value::from_obj(out);
            return true;
        }

        // __mul__ -> 新 list:整次重复(0 次得空表,浅拷同 __add__);乘数严格 int,负数报错不静默得空。
        // GC 走查同 __add__:唯一分配点 new_list,其后 reserve 加逐轮 copy_from 全程 trivial。
        bool fn___mul__(AriaVM& vm, Span<Value> slots) {
            const auto argc = slots.size() - 1;
            if (argc != 1) {
                return vm.arity_error(argc, 1);
            }
            if (!slots[1].is_int()) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a list and an integer, got {} and {}",
                               type_name(slots[0]), type_name(slots[1]));
            }
            const i64 count = slots[1].as_int();
            if (count < 0) {
                return vm.fail(ErrorCode::TypeMismatch, "__mul__ requires a non-negative integer, got {}", count);
            }
            const auto self = receiver<ObjList>(vm, slots[0]);
            if (self == nullptr) {
                return false;
            }
            const auto out = new_list(vm.gc());
            out->elements().reserve(self->elements().size() * static_cast<usize>(count));
            for (i64 i = 0; i < count; ++i) {
                out->elements().copy_from(self->elements());
            }
            slots[0] = Value::from_obj(out);
            return true;
        }

        // list 方法表。
        constexpr BuiltinFnEntry kListBuiltins[] = {
                {"init", fn_init},         {"push", fn_push},         {"pop", fn_pop},
                {"insert", fn_insert},     {"remove", fn_remove},     {"remove_at", fn_remove_at},
                {"clear", fn_clear},       {"sort", fn_sort},         {"reverse", fn_reverse},
                {"find", fn_find},         {"contains", fn_contains}, {"size", fn_size},
                {"is_empty", fn_is_empty}, {"join", fn_join},         {"iter", fn_iter},
                {"__add__", fn___add__},   {"__mul__", fn___mul__},
        };

    } // namespace

    ObjClass* ListClass::make_class(GC& gc, ObjClass* super) {
        // List bootstrap 类:内置 list 的语言方法面载体,类名与 type() 的类型名一致。
        const auto klass = new_class(gc, "List", super);
        Builtin::register_class_methods(gc, klass, kListBuiltins);
        return klass;
    }

} // namespace aria
