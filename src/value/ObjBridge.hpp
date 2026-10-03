#ifndef ARIA_OBJBRIDGE_HPP
#define ARIA_OBJBRIDGE_HPP

#include "object/ObjClosure.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // Value<->Object 桥接辅助(需两方完整类型的模板函数)的收口处;独立成头是为了让
    // Object.hpp 依赖不进入被编译层(常量池)广泛 include 的 Value.hpp,需要者显式 include。

    // 从 Value 一步取对象子类型(= is_obj + Object::try_as 合一):持对象且动态类型为 T 返回 T*,否则 nullptr。
    template<DerivedFromObj T>
    [[nodiscard]]
    T* try_as_obj(const Value value) noexcept {
        return value.is_obj() ? Object::try_as<T>(value.as_obj()) : nullptr;
    }

    // 类表成员值是否为「可调用」:闭包(字节码方法)或原生函数(内建方法)。
    [[nodiscard]]
    inline bool is_callable_value(const Value value) noexcept {
        if (!value.is_obj()) {
            return false;
        }
        const ObjType t = value.as_obj()->type();
        return t == ObjType::CLOSURE || t == ObjType::NATIVE_FN;
    }

    // 类表成员值是否为「方法」:defining class 戳定的方法闭包(MAKE_METHOD 注册时戳;
    // MAKE_STATIC/类上赋值不戳,故静态槽恒非方法)。
    [[nodiscard]]
    inline bool is_method(const Value value) noexcept {
        const auto closure = try_as_obj<ObjClosure>(value);
        return closure != nullptr && closure->is_method();
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
