#include "value/Value.hpp"

#include <bit>
#include <cmath>
#include <format>

#include "object/ObjString.hpp"
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
                    return lhs.as_obj()->equals(rhs.as_obj());
            }
            return false;
        }

        if (is_num(lhs) && is_num(rhs)) {
            return as_num(lhs) == as_num(rhs);
        }

        return false;
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
                return lhs.as_obj() == rhs.as_obj();
        }
        return false;
    }

    bool value_less(const Value lhs, const Value rhs) noexcept {
        // 数值域:混合升 f64;NaN 排在一切数值之前(双 NaN 互不小于,弱序下等价)。
        if (lhs.is_int() && rhs.is_int()) {
            return lhs.as_int() < rhs.as_int();
        }
        if (is_num(lhs) && is_num(rhs)) {
            const auto x = as_num(lhs);
            const auto y = as_num(rhs);
            if (std::isnan(x) || std::isnan(y)) {
                return std::isnan(x) && !std::isnan(y);
            }
            return x < y;
        }
        // 字符串域:无符号字节序,必须走 string_view::compare(手写逐 char 比较会把 0x80+ 字节排到 ASCII 之前)。
        const auto lhs_str = Object::as<ObjString>(lhs.as_obj());
        return lhs_str->view().compare(Object::as<ObjString>(rhs.as_obj())->view()) < 0;
    }

} // namespace aria
