#ifndef ARIA_STRING_CONSTANT_HPP
#define ARIA_STRING_CONSTANT_HPP

#include "common.hpp"

namespace aria {

    // VM 常量串注册表:VM 自己在**运行期按名取用**、须恒久存活的字符串常量,作单一事实源(枚举下标与拼写同源展开)。
    // **成员判据**:只收「VM 自己按名取用」的串。代码里写下的常量名(字段名/方法名/函数名等)不进本表 -- 它们编进
    // 常量池,经 ObjFunction::trace -> CodeUnit::trace 已随函数可达。
    // **为什么须恒久存活**:驻留池(InternPool)是 weak root,串无可达引用则下轮 collect 即被摘除,下次按名取用重铸
    // 一个新对象。算子钩子名正是这种串(实例算子派发每次都要一个稳定的 ObjString* 去查表),如今只被「恰好把它们
    // 当键的类表」间接保命 -- 那是巧合不是契约。故 AriaVM bootstrap 期一趟 new_string 把本表填进 string_constants_,
    // 并随 registers_ 一起在 VM 根 tracer 里标根:表在则串在。消费点统一经 AriaVM::string_constant(枚举)取用。
    // **命名**:算子/调用钩子取前后双下划线形 -- 普通 aria 标识符不会这么命名,故与用户自己的方法名不撞、一眼可辨是
    // 协议名;十一个名字与 Object::op_*_impl 虚函数族一一对应(那族回答「本对象上该算子/调用对应的可调用值」,
    // 内建类型直给自身实现)。`/` 是 aria 唯一的除法算子(无 // 形态),故取 __div__。
    // 形态同 ARIA_VALUE_REGISTER_LIST / ARIA_OPCODE_LIST:条目宏 define 顶格 + 容器单行内联 use + undef 紧随,
    // 注册表用毕即 #undef(其后只剩派生常量)。逐条注释用块注释(行注释会吞续行符)。**不另生成可读名表**
    //(kValueRegisterNames 那种是给反汇编打印 LOAD_REG 操作数用的):本表无字节码消费者,故无名表,只派生
    // 表长 kStringConstantCount(与 kValueRegisterCount 同式),供 VM 预置 string_constants_ 格位。
#define ARIA_STRING_CONSTANT_LIST(X)                            \
    X(OpAdd, "__add__")         /* 二元 + */                    \
    X(OpSub, "__sub__")         /* 二元 - */                    \
    X(OpMul, "__mul__")         /* 二元 * */                    \
    X(OpDiv, "__div__")         /* 二元 /(aria 唯一除法算子) */ \
    X(OpMod, "__mod__")         /* 二元 % */                    \
    X(OpLess, "__lt__")         /* 比较 < */                    \
    X(OpLessEqual, "__le__")    /* 比较 <= */                   \
    X(OpGreater, "__gt__")      /* 比较 > */                    \
    X(OpGreaterEqual, "__ge__") /* 比较 >= */                   \
    X(OpNegate, "__neg__")      /* 一元取负 */                  \
    X(OpCall, "__call__")       /* 调用钩子 */

#define ARIA_STRING_CONSTANT_ENUM(name, spelling) name,
    enum class StringConstant : u8 { ARIA_STRING_CONSTANT_LIST(ARIA_STRING_CONSTANT_ENUM) };
#undef ARIA_STRING_CONSTANT_ENUM

#define ARIA_STRING_CONSTANT_SPELLING(name, spelling) spelling,
    inline constexpr StringView kStringConstantSpellings[] = {ARIA_STRING_CONSTANT_LIST(ARIA_STRING_CONSTANT_SPELLING)};
#undef ARIA_STRING_CONSTANT_SPELLING

#undef ARIA_STRING_CONSTANT_LIST

    // 表长(= X 表行数):常量串表条数,替代对枚举稠密的依赖(同 kValueRegisterCount)。
    inline constexpr usize kStringConstantCount = std::size(kStringConstantSpellings);

} // namespace aria

#endif // ARIA_STRING_CONSTANT_HPP
