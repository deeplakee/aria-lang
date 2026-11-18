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
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 工厂构造以具体迭代器替换 call_class 预置的普通实例。
        bool fn_init(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 1) {
                return ctx.arity_error(argc, 1);
            }
            if (const auto list = try_as_obj<ObjList>(info[0])) {
                info.set_return_val(Value::from_obj(new_list_iterator(ctx.gc(), list)));
                return true;
            }
            if (const auto str = try_as_obj<ObjString>(info[0])) {
                info.set_return_val(Value::from_obj(new_string_iterator(ctx.gc(), str)));
                return true;
            }
            if (const auto map = try_as_obj<ObjMap>(info[0])) {
                info.set_return_val(Value::from_obj(new_map_iterator(ctx.gc(), map)));
                return true;
            }
            return ctx.fail(ErrorCode::TypeMismatch, "iterator source must be a string, list or map, got {}",
                            type_name(info[0]));
        }

        bool fn_has_next(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjIterator>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_bool(self->has_next()));
            return true;
        }

        bool fn_next(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjIterator>();
            if (self == nullptr) {
                return false;
            }
            const auto value = self->next(ctx);
            if (!value) {
                return false; // 已 fail(IterationExhausted)
            }
            info.set_return_val(*value);
            return true;
        }

        constexpr BuiltinFnEntry kIteratorBuiltins[] = {
                {"init", fn_init},
                {"has_next", fn_has_next},
                {"next", fn_next},
        };

    } // namespace

    ObjClass* IteratorClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "Iterator", super);
        Builtin::register_class_methods(gc, klass, kIteratorBuiltins);
        return klass;
    }

} // namespace aria
