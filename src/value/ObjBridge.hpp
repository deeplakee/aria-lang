#ifndef ARIA_OBJBRIDGE_HPP
#define ARIA_OBJBRIDGE_HPP

#include "object/Object.hpp"
#include "value/Value.hpp"

namespace aria {

    // Value<->Object 耦合辅助的收口处:同时依赖 Value 与 Object 完整类型、必须头内定义
    // (模板)的辅助函数落此;非模板的重依赖辅助(type_name(Value)/value_equal/value_hash/
    // format_value 系列等)已走「Value.hpp 声明 + Value.cpp 定义」旧路,依赖问题已解,不进本头。
    //
    //   独立成头而非常驻 Value.hpp:Object.hpp 依赖是头内定义的硬需求,而 Value.hpp 被编译层
    //   (常量池)广泛 include、「Object.hpp 依赖不污染 Value.hpp」是既有决策(见
    //   gc-implementation-plan Phase 2 笔记);本头的用户只在 runtime 层(builtin 守卫 /
    //   异常载荷判定等),故隔离在此,需要者显式 include。依赖方向 value -> object,与
    //   「Object.hpp 不 include Value.hpp」的既有约束互不冲突。

    // 从 Value 一步取对象子类型(try_obj = is_obj + Object::try_as 合一):v 持对象且动态类型
    // 匹配 T 时返回 T*,否则(非对象 / 类型不符)返回 nullptr。「守卫后使用」成对场景的一步
    // 形态,替代 `v.is_obj() && Object::is<T>(v.as_obj())` 命中后再 `Object::as<T>(v.as_obj())`
    // 的两步写法(类型只写一次)。分派臂内等静态已知场合仍走 v.is_obj() + Object::as<T>()。
    template<DerivedFromObj T>
    [[nodiscard]]
    T* try_obj(const Value& v) noexcept {
        return v.is_obj() ? Object::try_as<T>(v.as_obj()) : nullptr;
    }

    // 类表成员值是否为「可绑定方法」(M5):闭包(字节码方法)或原生函数(内建方法)--
    // uniform「可调用一律绑定」,绑定形态统一 ObjBoundMethod。决策 2「区分在值类型本身,
    // 表内无 tag」的判别谓词;非可调用静态值直读不缓存。**泛化扩展缝**:未来若再扩可绑定
    // 集合,改本谓词即可(类表是 Value 型,查找/trace/注册路径均不特化值类型)。
    // 收口三处消费:VM(LOAD_SUPER_METHOD 可调用检查 / MAKE_METHOD 槽形 ASSERT)、
    // ObjInstance::load_field(绑定判定)、ObjBoundMethod ctor ASSERT。
    [[nodiscard]]
    inline bool is_callable_value(const Value v) noexcept {
        if (!v.is_obj()) {
            return false;
        }
        const ObjType t = v.as_obj()->type();
        return t == ObjType::CLOSURE || t == ObjType::NATIVE_FN;
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
