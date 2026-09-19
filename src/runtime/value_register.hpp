#ifndef ARIA_VALUE_REGISTER_HPP
#define ARIA_VALUE_REGISTER_HPP

#include <iterator>

#include "common.hpp"

namespace aria {

    // 值寄存器组:VM 持有的单例值统一存放表(AriaVM::registers_,bootstrap 填充、set_vm_roots
    // 一趟循环标根),LOAD_REG n:u8 按索引把寄存器值压栈。寄存器只读(无 STORE_REG):编译代码
    // 不可写,写点全在 VM bootstrap。格节奏(设计见 .claude/reference/runtime/
    // collections-builtin-methods-plan.md §4.1「底座」):格随 VM 单例出生逐批登记(存储面);
    // LOAD_REG 发射按需(发射面,有字节码消费者的格才被编译侧引用,其余纯 C++ 存取)。
    //
    // 单一事实源(同 ARIA_ERROR_LIST / ARIA_OPCODE_LIST 风格):枚举声明、可读名表
    // kValueRegisterNames 与偏移常量 k<名字>Offset 同源展开,新增寄存器加一行 X(名字) 即收口,
    // 名字串经 # 派生。表长 kValueRegisterCount = 名表长度,两表同源无不同步风险。逐值注释
    // 用块注释(行注释会吞续行符)。
    //
    // 偏移常量 k<名字>Offset(如 kObjectClassOffset,值 = 枚举值,即寄存器组内格位):scoped
    // enum 不隐式转整型,C++ 侧数组下标与 LOAD_REG 操作数发射统一走常量,免逐点
    // std::to_underlying。
#define ARIA_VALUE_REGISTER_LIST(X)                                                                                \
    X(ObjectClass)   /* Object 根类(def 无 super 父类;VM bootstrap 填充,原 LOAD_OBJECT 收编) */                    \
    X(DefaultMark)   /* 缺参印章(私有 no-op native,call_closure 垫充未传槽;不注册 builtins 用户不可达) */          \
    X(MatchNoArm)    /* match 全臂未命中兜底异常(共享 ObjException;LOAD_REG + THROW 抛出) */                       \
    X(ListClass)     /* List bootstrap 类(内置 list 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */ \
    X(IteratorClass) /* Iterator bootstrap 类(迭代器的语言方法面 has_next/next,super 挂 Object 根;纯 C++ 存取) */  \
    X(MapClass)      /* Map bootstrap 类(内置 map 的语言方法面,super 挂 Object 根;纯 C++ 存取,无字节码消费者) */

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
