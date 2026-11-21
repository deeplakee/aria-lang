#include "runtime/stdlib/MathModule.hpp"

#include <cmath>
#include <limits>
#include <numbers>

#include "aria.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaExeContext.hpp"
#include "runtime/AriaVM.hpp"
#include "runtime/builtins/Builtin.hpp"
#include "value/Value.hpp"

namespace aria::stdlib {

    namespace {

        bool check_number_arguments(const NativeCallInfo& info, const u8 expected_count) {
            auto& ctx = info.ctx();
            if (const auto count = info.size(); count != expected_count) {
                return ctx.arity_error(count, expected_count);
            }
            for (usize index = 0; index < expected_count; ++index) {
                const Value value = info[index];
                if (!is_num(value)) {
                    return ctx.fail(ErrorCode::TypeMismatch, "argument must be a number, got {}", type_name(value));
                }
            }
            return true;
        }

        template<auto operation>
        bool unary_f64(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 1)) {
                return false;
            }
            info.set_return_val(Value::from_f64(operation(as_num(info[0]))));
            return true;
        }

        template<auto operation>
        bool binary_f64(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 2)) {
                return false;
            }
            info.set_return_val(Value::from_f64(operation(as_num(info[0]), as_num(info[1]))));
            return true;
        }

        bool fn_abs(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 1)) {
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

        bool fn_is_nan(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 1)) {
                return false;
            }
            info.set_return_val(Value::from_bool(std::isnan(as_num(info[0]))));
            return true;
        }

        bool fn_is_finite(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 1)) {
                return false;
            }
            info.set_return_val(Value::from_bool(std::isfinite(as_num(info[0]))));
            return true;
        }

        bool fn_is_inf(const NativeCallInfo& info) {
            if (!check_number_arguments(info, 1)) {
                return false;
            }
            info.set_return_val(Value::from_bool(std::isinf(as_num(info[0]))));
            return true;
        }

        constexpr BuiltinFnEntry kMathFunctions[] = {
                {"abs", fn_abs},
                {"floor", unary_f64<[](f64 value) { return std::floor(value); }>},
                {"ceil", unary_f64<[](f64 value) { return std::ceil(value); }>},
                {"trunc", unary_f64<[](f64 value) { return std::trunc(value); }>},
                {"round", unary_f64<[](f64 value) { return std::round(value); }>},

                {"sqrt", unary_f64<[](f64 value) { return std::sqrt(value); }>},
                {"cbrt", unary_f64<[](f64 value) { return std::cbrt(value); }>},
                {"pow", binary_f64<[](f64 base, f64 exponent) { return std::pow(base, exponent); }>},
                {"hypot", binary_f64<[](f64 lhs, f64 rhs) { return std::hypot(lhs, rhs); }>},
                {"exp", unary_f64<[](f64 value) { return std::exp(value); }>},
                {"log", unary_f64<[](f64 value) { return std::log(value); }>},
                {"log2", unary_f64<[](f64 value) { return std::log2(value); }>},
                {"log10", unary_f64<[](f64 value) { return std::log10(value); }>},

                {"sin", unary_f64<[](f64 value) { return std::sin(value); }>},
                {"cos", unary_f64<[](f64 value) { return std::cos(value); }>},
                {"tan", unary_f64<[](f64 value) { return std::tan(value); }>},
                {"asin", unary_f64<[](f64 value) { return std::asin(value); }>},
                {"acos", unary_f64<[](f64 value) { return std::acos(value); }>},
                {"atan", unary_f64<[](f64 value) { return std::atan(value); }>},
                {"atan2", binary_f64<[](f64 ordinate, f64 abscissa) { return std::atan2(ordinate, abscissa); }>},

                {"is_nan", fn_is_nan},
                {"is_finite", fn_is_finite},
                {"is_inf", fn_is_inf},
        };

    } // namespace

    void register_math_exports(AriaVM& host, ObjModule* module) {
        auto& gc      = host.gc();
        auto& globals = module->globals();
        globals.set(Value::from_obj(new_string(gc, "pi")), Value::from_f64(std::numbers::pi_v<f64>));
        globals.set(Value::from_obj(new_string(gc, "e")), Value::from_f64(std::numbers::e_v<f64>));
        globals.set(Value::from_obj(new_string(gc, "tau")), Value::from_f64(2.0 * std::numbers::pi_v<f64>));
        globals.set(Value::from_obj(new_string(gc, "inf")), Value::from_f64(std::numeric_limits<f64>::infinity()));
        globals.set(Value::from_obj(new_string(gc, "nan")), Value::from_f64(std::numeric_limits<f64>::quiet_NaN()));
        Builtin::register_functions(gc, globals, kMathFunctions);
    }

} // namespace aria::stdlib
