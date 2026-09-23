#ifndef ARIA_VALUE_REGISTER_HPP
#define ARIA_VALUE_REGISTER_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 值寄存器组:VM 持有的单例值统一存放表(AriaVM::registers_,bootstrap 填充、set_vm_roots 一趟循环标根),LOAD_REG n:u8
    // 按索引把寄存器值压栈。寄存器只读(无 STORE_REG):编译代码不可写,写点全在 VM bootstrap。存储面:VM 单例出生即登记一
    // 格(本表加一行 + bootstrap 填充一行);发射面:LOAD_REG 按需发射,有字节码消费者的格才被编译侧引用,其余纯 C++ 存取。
    // 单一事实源(同 ARIA_ERROR_LIST / ARIA_OPCODE_LIST 风格):枚举声明、可读名表 kValueRegisterNames 与偏移常量 k<名字>
    // Offset 同源展开,新增寄存器加一行 X(名字) 即收口,名字串经 # 派生。表长 kValueRegisterCount = 名表长度,两表同源无
    // 不同步风险。逐值注释用块注释(行注释会吞续行符)。偏移常量 k<名字>Offset(如 kObjectClassOffset,值 = 枚举值,即寄存
    // 器组内格位):scoped enum 不隐式转整型,C++ 侧数组下标与 LOAD_REG 操作数发射统一走常量,免逐点 std::to_underlying。
    // 末组 = 内置类型的算子实现缓存五格(String 的 `__add__`/`__lt__`/`__le__`/`__gt__`/`__ge__` 原生函数值):内置类型取
    // 实现走 Object::op_*_impl 的 override,直读实现格免每次过类表查找。实现格的规范家仍是类表(方法读路径 `"a".__add__`
    // 就查它),bootstrap 注册后即从类表拷入并 ASSERT 一致。实例侧的按名取实现不走寄存器:ObjInstance 的 override 经
    // AriaVM::string_constant 取常量串表(runtime/string_constant.hpp)里的钩子名,再按名到实例/类链查表。
#define ARIA_VALUE_REGISTER_LIST(X)                                                                                    \
    X(ObjectClass)   /* Object 根类(def 无 super 父类;VM bootstrap 填充) */                                            \
    X(DefaultMark)   /* 缺参印章(私有 no-op native,call_closure 垫充未传槽;不注册 builtins 用户不可达) */              \
    X(MatchNoArm)    /* match 全臂未命中兜底异常(共享 ObjException;LOAD_REG + THROW 抛出) */                           \
    X(IteratorClass) /* Iterator bootstrap 类(迭代器的语言方法面 has_next/next,super 挂 Object 根;纯 C++ 存取) */      \
    X(ListClass)     /* List bootstrap 类(内置 list 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */     \
    X(MapClass)      /* Map bootstrap 类(内置 map 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */       \
    X(StringClass)   /* String bootstrap 类(内置 string 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */ \
    X(RangeClass)    /* Range bootstrap 类(内置 range 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */   \
    X(StringAddFn)   /* __add__ 原生,从 String 类表拷入;op_add_impl 直读本格          */                               \
    X(StringLtFn)    /* __lt__ 原生,从 String 类表拷入;op_less_impl 直读本格          */                               \
    X(StringLeFn)    /* __le__ 原生,从 String 类表拷入;op_less_equal_impl 直读本格    */                               \
    X(StringGtFn)    /* __gt__ 原生,从 String 类表拷入;op_greater_impl 直读本格       */                               \
    X(StringGeFn)    /* __ge__ 原生,从 String 类表拷入;op_greater_equal_impl 直读本格 */

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

    // 表长(= X 表行数):寄存器数组容量,替代对枚举稠密(上界 = 末条枚举值)的依赖。
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
