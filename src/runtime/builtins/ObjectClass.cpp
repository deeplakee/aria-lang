#include "runtime/builtins/ObjectClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjString.hpp"
#include "object/Object.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        bool fn_init(const NativeCallInfo&) { return true; }

        // 默认显示委托 repr，不执行用户代码；子类可在类链上覆盖。
        bool fn___str__(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.self();
            if (!self.is_obj()) {
                return ctx.fail(ErrorCode::TypeMismatch, "type {} does not support '__str__'", type_name(self));
            }
            info.set_return_val(Value::from_obj(new_string(ctx.gc(), self.as_obj()->repr())));
            return true;
        }

        constexpr BuiltinFnEntry kObjectBuiltins[] = {
                {"init", fn_init},
                {"__str__", fn___str__},
        };

    } // namespace

    ObjClass* ObjectClass::make_class(GC& gc) {
        const auto klass = new_class(gc, "Object", nullptr);
        Builtin::register_class_methods(gc, klass, kObjectBuiltins);
        return klass;
    }

} // namespace aria
