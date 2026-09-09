#include "value/Value.hpp"

#include <bit>
#include <format>

#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjException.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjInstance.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjNativeFn.hpp"
#include "object/ObjString.hpp"
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

    String format_string(const ObjString* obj) { return std::format("\"{}\"", util::escape_string(obj->view())); }

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
        // 与 format_value 的关键区别:Obj 不经虚函数 to_string()(可重载、重入风险),改走非虚的
        // obj->type() 枚举分派。详见 Value.hpp 注释。
        switch (v.type()) {
            case Value::Type::Nil:
                return "nil";
            case Value::Type::Bool:
                return v.as_bool() ? "true" : "false";
            case Value::Type::Int:
                return std::format("{}", v.as_int());
            case Value::Type::F64:
                return format_f64(v.as_f64());
            case Value::Type::Obj: {
                switch (Object* obj = v.as_obj(); obj->type()) { // 非虚:读 type_ 字段,不经虚分派
                    case ObjType::STRING:
                        return format_string(Object::as<ObjString>(obj));
                    case ObjType::FUNCTION:
                        return std::format("<fn {}>", Object::as<ObjFunction>(obj)->name()->view());
                    case ObjType::CLOSURE:
                        // 闭包:渲染 `<fn name>`(与 ObjClosure::to_string 同文案,纯 C++ 访问器,非虚无重入风险)。
                        return std::format("<fn {}>", Object::as<ObjClosure>(obj)->function()->name()->view());
                    case ObjType::UPVALUE:
                        // Upvalue:语言层不可见的内部对象,渲染稳定短文案(地址型描述噪声大且地址不稳)。
                        return "<upvalue>";
                    case ObjType::MODULE:
                        return std::format("<module {}>", Object::as<ObjModule>(obj)->name()->view());
                    case ObjType::CLASS:
                        // 类:渲染 `<class Foo>`(与 ObjClass::to_string 同文案,纯 C++ 访问器,非虚无重入风险)。
                        return std::format("<class {}>", Object::as<ObjClass>(obj)->name()->view());
                    case ObjType::INSTANCE:
                        // 实例:渲染 `<Foo instance>`(与 ObjInstance::to_string 同文案)。
                        return std::format("<{} instance>", Object::as<ObjInstance>(obj)->cls()->name()->view());
                    case ObjType::BOUND_METHOD:
                        // 绑定方法:渲染 `<bound method m>`(与 ObjBoundMethod::to_string 同文案)。
                        return std::format("<bound method {}>",
                                           Object::as<ObjBoundMethod>(obj)->method()->function()->name()->view());
                    case ObjType::NATIVE_FN:
                        return std::format("<fn {}>", Object::as<ObjNativeFn>(obj)->name()->view());
                    case ObjType::EXCEPTION:
                        // ObjException:渲染完整烘焙消息(纯 C++ 访问器,非虚无重入风险;消息即身份,
                        // 同 to_string 文案,不带引号)。
                        return String{Object::as<ObjException>(obj)->message()->view()};
                    default:
                        // 未落地 / 用户类实例等:仅类型名 + 地址,绝不调用可重载的 to_string,杜绝重入 VM。
                        // 复用 Object::debug_repr()(非虚,不经虚分派)。
                        return obj->debug_repr();
                }
            }
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
