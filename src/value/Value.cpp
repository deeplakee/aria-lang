#include "value/Value.hpp"

#include <bit>
#include <format>

#include "object/Object.hpp"
#include "util/util.hpp"

namespace aria {

    u32 value_hash(const Value& v) noexcept {
        switch (v.type()) {
            case Value::Type::Nil:
                return 0x12345678u;
            case Value::Type::Bool:
                return v.as_bool() ? 0x9E3779B9u : 0x85EBCA6Bu;
            case Value::Type::Int:
                return util::hash_num(static_cast<u64>(v.as_int()));
            case Value::Type::F64:
                return util::hash_num(std::bit_cast<u64>(v.as_f64()));
            case Value::Type::Obj:
                return v.as_obj()->hash(); // ObjString:内容 FNV-1a;其它:地址哈希
        }
        return 0;
    }

    bool value_equal(const Value& a, const Value& b) noexcept {
        const auto ta = a.type();
        const auto tb = b.type();

        if (ta == tb) {
            switch (ta) {
                case Value::Type::Nil:
                    return true;
                case Value::Type::Bool:
                    return a.as_bool() == b.as_bool();
                case Value::Type::Int:
                    return a.as_int() == b.as_int();
                case Value::Type::F64:
                    // IEEE 数值:-0.0==0.0 true, NaN!=NaN(与 === 的按位不同)。
                    return a.as_f64() == b.as_f64();
                case Value::Type::Obj:
                    return a.as_obj()->equals(b.as_obj()); // 内容相等虚函数分派
            }
            return false;
        }

        // 跨类型:仅 Int<->F64 数值比较(bool/Obj/Nil 不参与)。
        if ((ta == Value::Type::Int && tb == Value::Type::F64) || (ta == Value::Type::F64 && tb == Value::Type::Int)) {
            const auto fa = ta == Value::Type::Int ? static_cast<f64>(a.as_int()) : a.as_f64();
            const auto fb = tb == Value::Type::Int ? static_cast<f64>(b.as_int()) : b.as_f64();
            return fa == fb;
        }

        return false; // 其余跨类型(nil/bool/obj 之间)不相等
    }

    String format_f64(const f64 d) {
        String s = std::format("{}", d);
        if (s.find_first_of(".eE") == String::npos && s.find("inf") == String::npos && s.find("nan") == String::npos) {
            s += ".0";
        }
        return s;
    }

    StringView type_name(const Value& v) noexcept {
        if (v.is_obj()) {
            return v.as_obj()->type_name();
        }
        return v.type_name();
    }

    String format_value(const Value& v) {
        switch (v.type()) {
            case Value::Type::Nil:
                return "nil";
            case Value::Type::Bool:
                return v.as_bool() ? "true" : "false";
            case Value::Type::Int:
                return std::format("{}", v.as_int());
            case Value::Type::F64:
                return format_f64(v.as_f64());
            case Value::Type::Obj:
                return v.as_obj()->to_string();
        }
        UNREACHABLE();
    }

    String format_value_debug(const Value& v) {
        // 与 format_value 的关键区别:Obj 走 debug_repr() 虚分派而非可重载的 to_string()
        //(后者是未来用户类 __str__ 的挂载点,可重入 VM)。debug_repr 虽是虚函数,但其
        // override 契约是纯 C++ 惰性渲染(绝不重入 VM / 不触 GC 回收,见 Object.hpp),语言层
        // 无法新增 C++ 子类型,故调试上下文虚分派安全,绝不触用户重载;各类型的 debug 文案
        // 由各子类型自己实现,本函数不再按 ObjType 分型。详见 Value.hpp 注释。
        switch (v.type()) {
            case Value::Type::Nil:
                return "nil";
            case Value::Type::Bool:
                return v.as_bool() ? "true" : "false";
            case Value::Type::Int:
                return std::format("{}", v.as_int());
            case Value::Type::F64:
                return format_f64(v.as_f64());
            case Value::Type::Obj:
                return v.as_obj()->debug_repr();
        }
        UNREACHABLE();
    }

    bool value_identical(const Value& a, const Value& b) noexcept {
        if (a.type() != b.type()) {
            return false;
        }
        switch (a.type()) {
            case Value::Type::Nil:
                return true;
            case Value::Type::Bool:
                return a.as_bool() == b.as_bool();
            case Value::Type::Int:
                return a.as_int() == b.as_int();
            case Value::Type::F64:
                return std::bit_cast<u64>(a.as_f64()) == std::bit_cast<u64>(b.as_f64());
            case Value::Type::Obj:
                return a.as_obj() == b.as_obj(); // 指针相等(intern 后等价内容串同指针)
        }
        return false;
    }

} // namespace aria
