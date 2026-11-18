#include "runtime/builtins/MapClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjList.hpp"
#include "object/ObjMap.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjMapIterator.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 键按 === 判等，整数 1 与浮点 1.0 是不同键。

        bool fn_init(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            info.set_return_val(Value::from_obj(new_map(ctx.gc())));
            return true;
        }

        bool fn_size(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_int(static_cast<i64>(self->table().size())));
            return true;
        }

        bool fn_is_empty(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->table().empty()));
            return true;
        }

        bool fn_has(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            const auto entry = self->table().find(info[0]);
            info.set_return_val(Value::from_bool(entry != nullptr));
            return true;
        }

        // 缺键返回 nil；要区分值为 nil 的表项须用 has。
        bool fn_get(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            const auto entry = self->table().find(info[0]);
            info.set_return_val(entry != nullptr ? entry->value : Value::nil_val());
            return true;
        }

        bool fn_remove(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->table().erase(info[0])));
            return true;
        }

        bool fn_clear(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            self->table().clear();
            info.set_return_val(Value::nil_val());
            return true;
        }

        bool fn_keys(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            const auto list = new_list(ctx.gc());
            for (const auto& [key, _]: self->table()) {
                list->elements().push(key);
            }
            info.set_return_val(Value::from_obj(list));
            return true;
        }

        // 快照顺序与 keys 对齐，均随占用槽顺序。
        bool fn_values(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            const auto list = new_list(ctx.gc());
            for (const auto& [_, value]: self->table()) {
                list->elements().push(value);
            }
            info.set_return_val(Value::from_obj(list));
            return true;
        }

        // 外层列表须跨内层列表分配保活；每个内层列表建成即发布到外层。
        bool fn_pairs(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            const auto pairs = new_list(ctx.gc());
            const auto guard = ctx.gc().make_guard(pairs);
            for (const auto& [key, value]: self->table()) {
                const auto pair = new_list(ctx.gc());
                pair->elements().push(key);
                pair->elements().push(value);
                pairs->elements().push(Value::from_obj(pair));
            }
            info.set_return_val(Value::from_obj(pairs));
            return true;
        }

        bool fn_iter(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjMap>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_obj(new_map_iterator(ctx.gc(), self)));
            return true;
        }

        constexpr BuiltinFnEntry kMapBuiltins[] = {
                {"init", fn_init},     {"size", fn_size},   {"is_empty", fn_is_empty}, {"has", fn_has},
                {"get", fn_get},       {"keys", fn_keys},   {"values", fn_values},     {"pairs", fn_pairs},
                {"remove", fn_remove}, {"clear", fn_clear}, {"iter", fn_iter},
        };

    } // namespace

    ObjClass* MapClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "Map", super);
        Builtin::register_class_methods(gc, klass, kMapBuiltins);
        return klass;
    }

} // namespace aria
