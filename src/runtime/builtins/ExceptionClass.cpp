#include "runtime/builtins/ExceptionClass.hpp"

#include <utility>

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjException.hpp"
#include "object/ObjInstance.hpp"
#include "object/Object.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/ObjBridge.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 内建异常读原生成员，用户子类实例读 _message/_code 字段。

        // 用户子类覆盖 init 后须自行填写异常字段，否则读取报 UndefinedProperty。
        bool fn_init(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto inst = try_as_obj<ObjInstance>(info.self());
            if (inst == nullptr) {
                return ctx.fail(ErrorCode::TypeMismatch, "init requires an instance receiver, got {}",
                                type_name(info.self()));
            }
            inst->store_field(ctx, ctx.str<"_code">(), Value::from_int(std::to_underlying(ErrorCode::Error)));
            inst->store_field(ctx, ctx.str<"_message">(), Value::from_obj(new_string(ctx.gc(), "")));
            return true;
        }

        bool fn_message(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            if (const auto ex = try_as_obj<ObjException>(info.self())) {
                info.set_return_val(Value::from_obj(ex->message()));
                return true;
            }
            if (const auto inst = try_as_obj<ObjInstance>(info.self())) {
                if (const auto hit = inst->load_field(ctx, ctx.str<"_message">())) {
                    info.set_return_val(*hit); // 建成即写槽:命中值经此根化
                    return true;
                }
                return false;
            }
            return ctx.fail(ErrorCode::TypeMismatch, "message requires an Exception receiver, got {}",
                            type_name(info.self()));
        }

        bool fn_code(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            if (const auto ex = try_as_obj<ObjException>(info.self())) {
                info.set_return_val(Value::from_int(ex->numeric_code()));
                return true;
            }
            if (const auto inst = try_as_obj<ObjInstance>(info.self())) {
                if (const auto hit = inst->load_field(ctx, ctx.str<"_code">())) {
                    info.set_return_val(*hit); // 建成即写槽:命中值经此根化
                    return true;
                }
                return false;
            }
            return ctx.fail(ErrorCode::TypeMismatch, "code requires an Exception receiver, got {}",
                            type_name(info.self()));
        }

        constexpr BuiltinFnEntry kExceptionBuiltins[] = {
                {"init", fn_init},
                {"message", fn_message},
                {"code", fn_code},
        };

    } // namespace

    ObjClass* ExceptionClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "Exception", super);
        Builtin::register_class_methods(gc, klass, kExceptionBuiltins);
        return klass;
    }

} // namespace aria
