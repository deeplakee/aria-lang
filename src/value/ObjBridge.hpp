#ifndef ARIA_OBJBRIDGE_HPP
#define ARIA_OBJBRIDGE_HPP

#include "object/ObjClosure.hpp" // is_method 判方法性需 ObjClosure 完整类型
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

    // 类表成员值是否为「可调用」(M5):闭包(字节码方法)或原生函数(内建方法)。
    // 2026-09-11 改定:**绑定判别退役出读路径**(原 uniform「可调用一律绑定」--读路径改判
    // is_method(Value) 即 defining class 戳;「可调用 ⟺ 方法」的值类型判据随函数值
    // 静态/静态方法落表失效,补记见 m5 计划)。本谓词保留为可调用集合的**泛化扩展缝**
    //(未来再扩可调用集合改本谓词即可;类表是 Value 型,查找/trace/注册路径均不特化值类型)。
    // 收口消费:ObjBoundMethod ctor ASSERT(方法值可调用守卫);原 VM 读路径(LOAD_SUPER_METHOD
    // 绑定判别/MAKE_METHOD 槽形 ASSERT)与 ObjInstance::load_field(绑定判定)三处消费随
    // 戳判别退役。
    [[nodiscard]]
    inline bool is_callable_value(const Value v) noexcept {
        if (!v.is_obj()) {
            return false;
        }
        const ObjType t = v.as_obj()->type();
        return t == ObjType::CLOSURE || t == ObjType::NATIVE_FN;
    }

    // 类表成员值是否为「方法」(M5,2026-09-11 改定):defining class 戳定的方法闭包
    // (MAKE_METHOD 注册时戳;MAKE_STATIC/类上赋值不戳 ⟹ 静态槽持函数值/lambda/原生恒
    // 非方法)。读路径(ObjInstance::load_field / LOAD_SUPER_FIELD)的绑定判别谓词 --
    // 判别不看值类型,一步收「取闭包 + 查戳」两步守卫。**泛化扩展缝**:未来再扩方法
    // 承载形态改本谓词即可。
    [[nodiscard]]
    inline bool is_method(const Value v) noexcept {
        const auto closure = try_obj<ObjClosure>(v);
        return closure != nullptr && closure->is_method();
    }

} // namespace aria

#endif // ARIA_OBJBRIDGE_HPP
