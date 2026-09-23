#ifndef ARIA_OBJBRIDGE_HPP
#define ARIA_OBJBRIDGE_HPP

#include "object/ObjClosure.hpp" // is_method 判方法性需 ObjClosure 完整类型
#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // Value<->Object 耦合辅助的收口处:同时依赖 Value 与 Object 完整类型、必须头内定义
    // (模板)的辅助函数落此;非模板的重依赖辅助(type_name(Value)/value_equal/value_hash/
    // format_value 系列等)走「Value.hpp 声明 + Value.cpp 定义」,不进本头。
    //
    //   独立成头:Object.hpp 依赖不进被编译层(常量池)广泛 include 的 Value.hpp(既有决策);
    //   本头的用户只在 runtime 层(builtin 守卫 / 异常载荷判定等),需要者显式 include。

    // 从 Value 一步取对象子类型(= is_obj + Object::try_as 合一):value 持对象且动态类型
    // 匹配 T 时返回 T*,否则 nullptr。「守卫后使用」成对场景的一步形态,替代「is<T> 命中后
    // 再 as<T>」的两步写法(类型只写一次);分派臂内等静态已知场合仍走 is_obj() + as<T>()。
    template<DerivedFromObj T>
    [[nodiscard]]
    T* try_obj(const Value value) noexcept {
        return value.is_obj() ? Object::try_as<T>(value.as_obj()) : nullptr;
    }

    // 类表成员值是否为「可调用」:闭包(字节码方法)或原生函数(内建方法)。读路径的绑定判别
    // 用 is_method;本谓词是可调用集合的**泛化扩展缝**(再扩可调用集合改本谓词即可)。
    // 消费:ObjBoundMethod ctor ASSERT 守卫。
    [[nodiscard]]
    inline bool is_callable_value(const Value value) noexcept {
        if (!value.is_obj()) {
            return false;
        }
        const ObjType t = value.as_obj()->type();
        return t == ObjType::CLOSURE || t == ObjType::NATIVE_FN;
    }

    // 类表成员值是否为「方法」:defining class 戳定的方法闭包(MAKE_METHOD 注册时戳;
    // MAKE_STATIC/类上赋值不戳 ⟹ 静态槽恒非方法)。读路径(ObjInstance::load_field /
    // LOAD_SUPER_FIELD)的绑定判别谓词:判别不看值类型,一步收「取闭包 + 查戳」两步守卫。
    // **泛化扩展缝**:未来再扩方法承载形态改本谓词即可。
    [[nodiscard]]
    inline bool is_method(const Value value) noexcept {
        const auto closure = try_obj<ObjClosure>(value);
        return closure != nullptr && closure->is_method();
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
