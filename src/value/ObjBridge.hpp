#ifndef ARIA_OBJBRIDGE_HPP
#define ARIA_OBJBRIDGE_HPP

#include "object/ObjFunction.hpp"
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // Value<->Object 桥接辅助(需两方完整类型的模板函数)的收口处;独立成头是为了让
    // Object.hpp 依赖不进入被编译层(常量池)广泛 include 的 Value.hpp,需要者显式 include。

    // 从 Value 一步取对象子类型(= is_obj + try_as 合一):持对象且动态类型为 T 返回 T*,否则 nullptr。
    template<DerivedFromObj T>
    [[nodiscard]]
    T* try_as_obj(const Value value) noexcept {
        return value.is_obj() ? value.as_obj()->try_as<T>() : nullptr;
    }

    // 取需要绑定接收者的实例方法;普通函数和已绑定方法均不命中。
    [[nodiscard]]
    inline ObjFunction* try_as_method(const Value value) noexcept {
        const auto function = try_as_obj<ObjFunction>(value);
        if (function && function->is_method()) {
            return function;
        }
        return nullptr;
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
