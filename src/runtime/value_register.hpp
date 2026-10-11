#ifndef ARIA_VALUE_REGISTER_HPP
#define ARIA_VALUE_REGISTER_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    class ObjClass; // 类段 type 列引用(展开点须自带前置声明或完整类型)

    // 值寄存器注册表:枚举、名表与偏移常量同源展开,新增寄存器加一行即收口(寄存器只读,写点全在
    // VM bootstrap)。两段子表直展开:单例段 = 字节码可见格(LOAD_REG 操作数域),类段 = bootstrap 类
    // 格(AriaVM 访问器与 builtins_ 曝光的生成源);合成表只作枚举/名表/格位的派生源,段序即枚举序
    //(字节码格位 0/1/2 不变)。type 列为格元素类型(宏内统一加 *),两段子表保持展开可见
    //(AriaVM.hpp/.cpp 经它生成访问器与曝光)。

#define ARIA_VALUE_REGISTER_SINGLETON_LIST(X)                                                                  \
    X(DefaultMark, default_mark, Object) /* missing-argument stamp (private ObjClass identity token fills      \
                             unfilled slots; not user-reachable) */                                            \
    X(MatchNoArm, match_no_arm, Object)  /* fallback exception when no match arm matches (shared ObjException; \
                               LOAD_REG + THROW) */

#define ARIA_VALUE_REGISTER_CLASS_LIST(X)                                                                            \
    X(ObjectClass, object_class, ObjClass)       /* Object root class (base of a def with no super) */               \
    X(ExceptionClass, exception_class, ObjClass) /* Exception bootstrap class (message/code; super is Object) */     \
    X(IteratorClass, iterator_class, ObjClass)   /* Iterator bootstrap class (has_next/next; super is Object) */     \
    X(ListClass, list_class, ObjClass)           /* List bootstrap class (built-in list methods; super is Object) */ \
    X(MapClass, map_class, ObjClass)             /* Map bootstrap class (built-in map methods; super is Object) */   \
    X(StringClass, string_class, ObjClass) /* String bootstrap class (built-in string methods; super is Object) */   \
    X(RangeClass, range_class, ObjClass)   /* Range bootstrap class (built-in range methods; super is Object) */

#define ARIA_VALUE_REGISTER_LIST(X) ARIA_VALUE_REGISTER_SINGLETON_LIST(X) ARIA_VALUE_REGISTER_CLASS_LIST(X)

#define ARIA_VALUE_REGISTER_ENUM(name, accessor, type) name,
    enum class ValueRegister : u8 { ARIA_VALUE_REGISTER_LIST(ARIA_VALUE_REGISTER_ENUM) };
#undef ARIA_VALUE_REGISTER_ENUM

#define ARIA_VALUE_REGISTER_NAME(name, accessor, type) #name,
    inline constexpr StringView kValueRegisterNames[] = {ARIA_VALUE_REGISTER_LIST(ARIA_VALUE_REGISTER_NAME)};
#undef ARIA_VALUE_REGISTER_NAME

#define ARIA_VALUE_REGISTER_OFFSET(name, accessor, type) \
    inline constexpr u8 k##name##Offset = std::to_underlying(ValueRegister::name);
    ARIA_VALUE_REGISTER_LIST(ARIA_VALUE_REGISTER_OFFSET)
#undef ARIA_VALUE_REGISTER_OFFSET

#undef ARIA_VALUE_REGISTER_LIST

    // 表长(= X 表行数):值寄存器组格数,替代对枚举稠密(上界 = 末条枚举值)的依赖。
    inline constexpr usize kValueRegisterCount = std::size(kValueRegisterNames);

    // 寄存器可读名(如 "ObjectClass"),反汇编注释与调试用。
    [[nodiscard]]
    constexpr StringView to_string(const ValueRegister reg) noexcept {
        const auto index = std::to_underlying(reg);
        ASSERT(index < std::size(kValueRegisterNames), "ValueRegister out of range");
        return kValueRegisterNames[index];
    }

} // namespace aria

#endif // ARIA_VALUE_REGISTER_HPP
