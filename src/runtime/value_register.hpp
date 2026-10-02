#ifndef ARIA_VALUE_REGISTER_HPP
#define ARIA_VALUE_REGISTER_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 值寄存器注册表:枚举、名表与偏移常量三表同源展开,新增寄存器加一行即收口(寄存器只读,写点全在 VM bootstrap)。
#define ARIA_VALUE_REGISTER_LIST(X)                                                                                  \
    X(DefaultMark)    /* missing-argument stamp (private ObjClass identity token fills unfilled slots; not           \
                         user-reachable) */                                                                          \
    X(MatchNoArm)     /* fallback exception when no match arm matches (shared ObjException; LOAD_REG + THROW) */     \
    X(ObjectClass)    /* Object root class (base of a def with no super; filled at bootstrap) */                     \
    X(ExceptionClass) /* Exception bootstrap class (message/code; super is Object) */                                \
    X(IteratorClass)  /* Iterator bootstrap class (has_next/next; super is Object) */                                \
    X(ListClass)      /* List bootstrap class (built-in list methods; super is Object) */                            \
    X(MapClass)       /* Map bootstrap class (built-in map methods; super is Object) */                              \
    X(StringClass)    /* String bootstrap class (built-in string methods; super is Object) */                        \
    X(RangeClass)     /* Range bootstrap class (built-in range methods; super is Object) */                          \
    X(StringLtFn)     /* __lt__ native, copied from the String class table; op_less_impl reads this cell          */ \
    X(StringLeFn)     /* __le__ native, copied from the String class table; op_less_equal_impl reads this cell    */ \
    X(StringGtFn)     /* __gt__ native, copied from the String class table; op_greater_impl reads this cell       */ \
    X(StringGeFn)     /* __ge__ native, copied from the String class table; op_greater_equal_impl reads this cell */ \
    X(StringAddFn)    /* __add__ native, copied from the String class table; op_add_impl reads this cell          */ \
    X(StringMulFn)    /* __mul__ native, copied from the String class table; op_mul_impl reads this cell          */ \
    X(ListAddFn)      /* __add__ native, copied from the List class table; op_add_impl reads this cell            */ \
    X(ListMulFn)      /* __mul__ native, copied from the List class table; op_mul_impl reads this cell            */

#define ARIA_VALUE_REGISTER_ENUM(name) name,
    enum class ValueRegister : u8 { ARIA_VALUE_REGISTER_LIST(ARIA_VALUE_REGISTER_ENUM) };
#undef ARIA_VALUE_REGISTER_ENUM

#define ARIA_VALUE_REGISTER_NAME(name) #name,
    inline constexpr StringView kValueRegisterNames[] = {ARIA_VALUE_REGISTER_LIST(ARIA_VALUE_REGISTER_NAME)};
#undef ARIA_VALUE_REGISTER_NAME

#define ARIA_VALUE_REGISTER_OFFSET(name) inline constexpr u8 k##name##Offset = std::to_underlying(ValueRegister::name);
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
