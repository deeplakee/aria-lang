#ifndef ARIA_EXCEPTION_CLASS_HPP
#define ARIA_EXCEPTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Exception bootstrap 类方法面(Exception bootstrap 类表条目)的宿主类:唯一成员是注册口,
    // 方法体与方法表仍住 ExceptionClass.cpp 匿名命名空间。与其余 bootstrap 类不同:本类经
    // builtins_ 的 "Exception" 键暴露(用户可继承)。VM 只在 bootstrap_exception_class 编排
    // 调用;须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    class ExceptionClass {
    public:
        static void register_methods(GC& gc, ObjClass* klass);
    };

} // namespace aria

#endif // ARIA_EXCEPTION_CLASS_HPP
