#include "runtime/builtins/ListClass.hpp"

#include <algorithm>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjListIterator.hpp"
#include "runtime/Eval.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "util/util.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        bool fn_init(const NativeCallInfo& info) {
            const auto& ctx  = info.ctx();
            const auto  list = new_list(ctx.gc());
            for (u8 i = 0; i < info.size(); ++i) {
                list->elements().push(info[i]);
            }
            info.set_return_val(Value::from_obj(list));
            return true;
        }

        bool fn_push(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            self->elements().push(info[0]); // trivial 分配不触 GC
            info.set_return_val(Value::nil_val());
            return true;
        }

        // 空表须报错，nil 可能是合法元素，不能充当失败哨兵。
        bool fn_pop(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            auto& list = self->elements();
            if (list.empty()) {
                return ctx.fail(ErrorCode::IndexOutOfBounds, "pop from empty list");
            }
            info.set_return_val(list[list.size() - 1]);
            list.pop(); // 搬移时 receiver 已离槽 0,trivial 无 GC 点
            return true;
        }

        // 负下标从尾计数，上界 size 表示追加。
        bool fn_insert(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 2) {
                return ctx.arity_error(argc, 2);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "insert index must be an integer, got {}", type_name(info[0]));
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            const auto position = util::resolve_position(info[0].as_int(), self->elements().size());
            if (!position) {
                return ctx.fail(ErrorCode::IndexOutOfBounds, "insert index {} out of range", info[0].as_int());
            }
            self->elements().insert(*position, info[1]);
            info.set_return_val(Value::nil_val());
            return true;
        }

        // 移除全部 == 命中元素。
        bool fn_remove(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->elements().remove(info[0])));
            return true;
        }

        bool fn_remove_at(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "remove_at index must be an integer, got {}",
                                type_name(info[0]));
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            auto&      list = self->elements();
            const auto slot = util::resolve_index(info[0].as_int(), list.size());
            if (!slot) {
                return ctx.fail(ErrorCode::IndexOutOfBounds, "remove_at index {} out of range", info[0].as_int());
            }
            info.set_return_val(list[*slot]);
            list.remove_at(*slot);
            return true;
        }

        bool fn_clear(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            self->elements().clear();
            info.set_return_val(Value::nil_val());
            return true;
        }

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

        // 先检查整表可比较性，再稳定排序，避免失败时留下部分排序结果。
        bool fn_sort(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            auto& list = self->elements();
            if (!list.empty()) {
                const auto domain = sort_domain(list[0]);
                if (domain == SortDomain::NotComparable) {
                    return ctx.fail(ErrorCode::TypeMismatch, "sort requires all numbers or all strings, got {}",
                                    type_name(list[0]));
                }
                for (usize index = 1; index < list.size(); ++index) {
                    if (sort_domain(list[index]) != domain) {
                        return ctx.fail(ErrorCode::TypeMismatch,
                                        "sort requires all numbers or all strings, got {} and {}", type_name(list[0]),
                                        type_name(list[index]));
                    }
                }
            }
            std::ranges::stable_sort(list, value_less);
            info.set_return_val(Value::nil_val());
            return true;
        }

        bool fn_reverse(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            std::ranges::reverse(self->elements()); // trivial 交换,无 GC 点
            info.set_return_val(Value::nil_val());
            return true;
        }

        bool fn_find(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            const auto hit = self->elements().find(info[0]);
            info.set_return_val(hit ? Value::from_int(static_cast<i64>(*hit)) : Value::nil_val());
            return true;
        }

        bool fn_contains(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->elements().contains(info[0])));
            return true;
        }

        bool fn_size(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_int(static_cast<i64>(self->elements().size())));
            return true;
        }

        bool fn_is_empty(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->elements().empty()));
            return true;
        }

        bool fn_join(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto sep = try_as_obj<ObjString>(info[0]);
            if (sep == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "join separator must be a string, got {}", type_name(info[0]));
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            const auto s = Eval::concat_values(ctx, self->elements(), sep->view());
            if (!s) {
                return false;
            }
            info.set_return_val(Value::from_obj(s));
            return true;
        }

        bool fn_iter(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_obj(new_list_iterator(ctx.gc(), self)));
            return true;
        }

        // 浅拷贝，两表中的嵌套对象仍共享。
        bool fn___add__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto rhs = try_as_obj<ObjList>(info[0]);
            if (rhs == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "__add__ requires two lists, got {} and {}",
                                type_name(info.self()), type_name(info[0]));
            }
            const auto lhs = info.receiver<ObjList>();
            if (lhs == nullptr) {
                return false;
            }
            const auto out = new_list(ctx.gc());
            out->elements().reserve(lhs->elements().size() + rhs->elements().size());
            out->elements().copy_from(lhs->elements());
            out->elements().copy_from(rhs->elements());
            info.set_return_val(Value::from_obj(out));
            return true;
        }

        bool fn___mul__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            if (!info[0].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "__mul__ requires a list and an integer, got {} and {}",
                                type_name(info.self()), type_name(info[0]));
            }
            const i64 count = info[0].as_int();
            if (count < 0) {
                return ctx.fail(ErrorCode::TypeMismatch, "__mul__ requires a non-negative integer, got {}", count);
            }
            const auto self = info.receiver<ObjList>();
            if (self == nullptr) {
                return false;
            }
            const auto out = new_list(ctx.gc());
            out->elements().reserve(self->elements().size() * static_cast<usize>(count));
            for (i64 i = 0; i < count; ++i) {
                out->elements().copy_from(self->elements());
            }
            info.set_return_val(Value::from_obj(out));
            return true;
        }

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
        const auto klass = new_class(gc, "List", super);
        Builtin::register_class_methods(gc, klass, kListBuiltins);
        return klass;
    }

} // namespace aria
