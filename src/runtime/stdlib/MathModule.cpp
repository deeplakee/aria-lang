#include "runtime/stdlib/MathModule.hpp"

#include <cmath>
#include <numbers>

#include "aria.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaExeContext.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria::stdlib {

    namespace {

        bool check_number_argument(const NativeCallInfo& info) {
            auto& ctx = info.ctx();
            if (const auto count = info.size(); count != 1) {
                return ctx.arity_error(count, 1);
            }
            if (!is_num(info[0])) {
                return ctx.fail(ErrorCode::TypeMismatch, "argument must be a number, got {}", type_name(info[0]));
            }
            return true;
        }

        bool fn_abs(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            const Value value = info[0];
            if (value.is_int()) {
                const i64 integer = value.as_int();
                if (integer == kIntMin) {
                    return info.ctx().fail(ErrorCode::IntegerOverflow, "integer result out of range");
                }
                info.set_return_val(Value::from_int(integer < 0 ? -integer : integer));
            } else {
                info.set_return_val(Value::from_f64(std::fabs(value.as_f64())));
            }
            return true;
        }

        bool fn_sqrt(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            info.set_return_val(Value::from_f64(std::sqrt(as_num(info[0]))));
            return true;
        }

        bool fn_floor(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            info.set_return_val(Value::from_f64(std::floor(as_num(info[0]))));
            return true;
        }

        bool fn_ceil(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            info.set_return_val(Value::from_f64(std::ceil(as_num(info[0]))));
            return true;
        }

        bool fn_is_nan(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            info.set_return_val(Value::from_bool(std::isnan(as_num(info[0]))));
            return true;
        }

        bool fn_is_finite(const NativeCallInfo& info) {
            if (!check_number_argument(info)) {
                return false;
            }
            info.set_return_val(Value::from_bool(std::isfinite(as_num(info[0]))));
            return true;
        }

        constexpr BuiltinFnEntry kMathFunctions[] = {
                {"abs", fn_abs},   {"sqrt", fn_sqrt},     {"floor", fn_floor},
                {"ceil", fn_ceil}, {"is_nan", fn_is_nan}, {"is_finite", fn_is_finite},
        };

    } // namespace

    void register_math_exports(GC& gc, ObjModule* module) {
        auto& globals = module->globals();
        globals.set(Value::from_obj(new_string(gc, "pi")), Value::from_f64(std::numbers::pi_v<f64>));
        globals.set(Value::from_obj(new_string(gc, "e")), Value::from_f64(std::numbers::e_v<f64>));
        Builtin::register_functions(gc, globals, kMathFunctions);
    }

} // namespace aria::stdlib
