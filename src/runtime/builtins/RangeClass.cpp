#include "runtime/builtins/RangeClass.hpp"

#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjRange.hpp"
#include "object/Object.hpp"
#include "object/iterator/ObjRangeIterator.hpp"
#include "runtime/ObjMovement.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria {

    namespace {

        // 一个端点表示无上界；两个端点表示不含上界。
        bool fn_init(const NativeCallInfo& info) {
            auto&      ctx  = info.ctx();
            const auto argc = info.size();
            if (argc != 1 && argc != 2) {
                return ctx.arity_error_range(argc, 1, 2);
            }
            if (argc == 1) {
                if (!info[0].is_int()) {
                    return ctx.fail(ErrorCode::TypeMismatch, "range bound must be an integer, got {}",
                                    type_name(info[0]));
                }
                info.set_return_val(Value::from_obj(new_range(ctx.gc(), info[0].as_int())));
                return true;
            }
            if (!info[0].is_int() || !info[1].is_int()) {
                return ctx.fail(ErrorCode::TypeMismatch, "range bounds must be integers, got {} and {}",
                                type_name(info[0]), type_name(info[1]));
            }
            info.set_return_val(Value::from_obj(new_range(ctx.gc(), info[0].as_int(), info[1].as_int(), true)));
            return true;
        }

        bool fn_iter(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto argc = info.size(); argc != 0) {
                return ctx.arity_error(argc, 0);
            }
            const auto self = info.receiver<ObjRange>();
            if (self == nullptr) {
                return false;
            }
            info.set_return_val(Value::from_obj(new_range_iterator(ctx.gc(), self)));
            return true;
        }

        constexpr BuiltinFnEntry kRangeBuiltins[] = {
                {"init", fn_init},
                {"iter", fn_iter},
        };

    } // namespace

    ObjClass* RangeClass::make_class(GC& gc, ObjClass* super) {
        const auto klass = new_class(gc, "Range", super);
        Builtin::register_class_methods(gc, klass, kRangeBuiltins);
        return klass;
    }

} // namespace aria
