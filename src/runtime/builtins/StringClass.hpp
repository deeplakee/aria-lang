#ifndef ARIA_STRING_CLASS_HPP
#define ARIA_STRING_CLASS_HPP

#include "common.hpp"
#include "runtime/string_constant.hpp" // StringConstant(算子钩子名)
#include "runtime/value_register.hpp"  // kString*FnOffset(寄存器实现格偏移)

namespace aria {

    class GC;
    class ObjClass;

    // string 方法面(String bootstrap 类表条目)的宿主类:注册口 + 算子实现缓存清单,方法体与方法表
    // 仍住 StringClass.cpp 匿名命名空间(方法面是 VM 侧语言面,object 层保持纯表示)。VM 只在
    // bootstrap_string_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入表/入格即根)。
    class StringClass {
    public:
        // 注册口:按名把方法表装进 String bootstrap 类(经 Builtin::register_class_methods 底座)。
        static void register_methods(GC& gc, ObjClass* klass);

        // 算子实现缓存清单:六个算子钩子名 -> AriaVM 寄存器实现格,bootstrap_string_class 装载类表后
        // 据此逐条把实现拷进 StringAddFn..StringMulFn 格(算子派发热路径直读,免每次过类表查找;类表
        // 仍是规范家,bootstrap 后无写点故两份恒一致,DEBUG 缺格即断言)。
        static constexpr Pair<StringConstant, u8> kOperatorFns[] = {
                {StringConstant::OpLess, kStringLtFnOffset},    {StringConstant::OpLessEqual, kStringLeFnOffset},
                {StringConstant::OpGreater, kStringGtFnOffset}, {StringConstant::OpGreaterEqual, kStringGeFnOffset},
                {StringConstant::OpAdd, kStringAddFnOffset},    {StringConstant::OpMul, kStringMulFnOffset},
        };
    };

} // namespace aria

#endif // ARIA_STRING_CLASS_HPP
