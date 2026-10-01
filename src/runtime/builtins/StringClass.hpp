#ifndef ARIA_STRING_CLASS_HPP
#define ARIA_STRING_CLASS_HPP

#include "common.hpp"
#include "runtime/value_register.hpp" // kString*FnOffset(寄存器实现格偏移)

namespace aria {

    class GC;
    class ObjClass;

    // string 方法面(String bootstrap 类)的自持单元:**公有面 = make_class 加算子实现缓存清单**。
    // 方法体与方法表仍住 StringClass.cpp 匿名命名空间(方法面是 VM 侧语言面,object 层保持纯表示)。
    // VM 只在 ctor 构造临界区内调用(创建免守卫,入寄存器/入格即根)。
    class StringClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 StringClass 格由
        // 调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);

        // 算子实现缓存清单(六个算子钩子名 -> String 实现格;契约与消费见 runtime/value_register.hpp
        // 表头,键为钩子拼写)。
        static constexpr Pair<StringView, u8> kOperatorFns[] = {
                {"__lt__", kStringLtFnOffset}, {"__le__", kStringLeFnOffset},   {"__gt__", kStringGtFnOffset},
                {"__ge__", kStringGeFnOffset}, {"__add__", kStringAddFnOffset}, {"__mul__", kStringMulFnOffset},
        };
    };

} // namespace aria

#endif // ARIA_STRING_CLASS_HPP
