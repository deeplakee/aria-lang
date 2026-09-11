#include "value/Value.hpp"

#include <bit>
#include <format>

#include "object/Object.hpp"
#include "util/util.hpp"

namespace aria {

    u32 value_hash(const Value value) noexcept {
        switch (value.type()) {
            case Value::Type::Nil:
                return 0x12345678u;
            case Value::Type::Bool:
                return value.as_bool() ? 0x9E3779B9u : 0x85EBCA6Bu;
            case Value::Type::Int:
                return util::hash_num(static_cast<u64>(value.as_int()));
            case Value::Type::F64:
                return util::hash_num(std::bit_cast<u64>(value.as_f64()));
            case Value::Type::Obj:
                return value.as_obj()->hash(); // ObjString:内容 FNV-1a;其它:地址哈希
        }
        return 0;
    }

    bool value_equal(const Value lhs, const Value rhs) noexcept {
        const auto lhs_type = lhs.type();
        const auto rhs_type = rhs.type();

        if (lhs_type == rhs_type) {
            switch (lhs_type) {
                case Value::Type::Nil:
                    return true;
                case Value::Type::Bool:
                    return lhs.as_bool() == rhs.as_bool();
                case Value::Type::Int:
                    return lhs.as_int() == rhs.as_int();
                case Value::Type::F64:
                    // IEEE 数值:-0.0==0.0 true, NaN!=NaN(与 === 的按位不同)。
                    return lhs.as_f64() == rhs.as_f64();
                case Value::Type::Obj:
                    return lhs.as_obj()->equals(rhs.as_obj()); // 内容相等虚函数分派
            }
            return false;
        }

        // 跨类型:仅 Int<->F64 数值比较(bool/Obj/Nil 不参与)。
        if ((lhs_type == Value::Type::Int && rhs_type == Value::Type::F64) ||
            (lhs_type == Value::Type::F64 && rhs_type == Value::Type::Int)) {
            const auto lhs_f64 = lhs_type == Value::Type::Int ? static_cast<f64>(lhs.as_int()) : lhs.as_f64();
            const auto rhs_f64 = rhs_type == Value::Type::Int ? static_cast<f64>(rhs.as_int()) : rhs.as_f64();
            return lhs_f64 == rhs_f64;
        }

        return false; // 其余跨类型(nil/bool/obj 之间)不相等
    }

    String format_f64(const f64 value) {
        String s = std::format("{}", value);
        if (s.find_first_of(".eE") == String::npos && s.find("inf") == String::npos && s.find("nan") == String::npos) {
            s += ".0";
        }
        return s;
    }

    StringView type_name(const Value value) noexcept {
        if (value.is_obj()) {
            return value.as_obj()->type_name();
        }
        return value.type_name();
    }

    String format_value(const Value value) {
        switch (value.type()) {
            case Value::Type::Nil:
                return "nil";
            case Value::Type::Bool:
                return value.as_bool() ? "true" : "false";
            case Value::Type::Int:
                return std::format("{}", value.as_int());
            case Value::Type::F64:
                return format_f64(value.as_f64());
            case Value::Type::Obj:
                return value.as_obj()->to_string();
        }
        UNREACHABLE();
    }

    String format_value_debug(const Value value) {
        // Obj 走 debug_repr() 而非可重载的 to_string():调试上下文不可重入 VM。详见 Value.hpp 注释。
        switch (value.type()) {
            case Value::Type::Nil:
                return "nil";
            case Value::Type::Bool:
                return value.as_bool() ? "true" : "false";
            case Value::Type::Int:
                return std::format("{}", value.as_int());
            case Value::Type::F64:
                return format_f64(value.as_f64());
            case Value::Type::Obj:
                return value.as_obj()->debug_repr();
        }
        UNREACHABLE();
    }

    bool value_identical(const Value lhs, const Value rhs) noexcept {
        if (lhs.type() != rhs.type()) {
            return false;
        }
        switch (lhs.type()) {
            case Value::Type::Nil:
                return true;
            case Value::Type::Bool:
                return lhs.as_bool() == rhs.as_bool();
            case Value::Type::Int:
                return lhs.as_int() == rhs.as_int();
            case Value::Type::F64:
                return std::bit_cast<u64>(lhs.as_f64()) == std::bit_cast<u64>(rhs.as_f64());
            case Value::Type::Obj:
                return lhs.as_obj() == rhs.as_obj(); // 指针相等(intern 后等价内容串同指针)
        }
        return false;
    }

} // namespace aria
