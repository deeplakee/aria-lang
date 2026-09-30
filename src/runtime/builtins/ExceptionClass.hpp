#ifndef ARIA_EXCEPTION_CLASS_HPP
#define ARIA_EXCEPTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Exception bootstrap 类的自持单元:**唯一公有口是 make_class**。方法体与方法表仍住
    // ExceptionClass.cpp 匿名命名空间。与其余 bootstrap 类不同:本类经 builtins_ 的 "Exception"
    // 键暴露、用户可继承(键写点在 AriaVM 侧,不经 make_class)。VM 只在 bootstrap_exception_class
    // 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入寄存器即根)。
    class ExceptionClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 ExceptionClass 格由
        // 调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_EXCEPTION_CLASS_HPP
