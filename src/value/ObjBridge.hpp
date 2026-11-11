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

    // 类表成员值是否为「可调用」:闭包(字节码方法)或原生函数(内建方法)。
    [[nodiscard]]
    inline bool is_callable(const Value value) noexcept {
        if (!value.is_obj()) {
            return false;
        }
        return value.as_obj()->type() == ObjType::FUNCTION;
    }

    // 需要接收者的未绑定实例方法:脚本闭包与原生函数均以 defining class 判定。
    // 普通/静态函数不戳,已有绑定方法不重复绑定;成员赋值不改变函数值携带的标记。
    [[nodiscard]]
    inline bool is_method(const Value value) noexcept {
        if (const auto function = try_as_obj<ObjFunction>(value)) {
            return function->is_method();
        }
        return false;
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
