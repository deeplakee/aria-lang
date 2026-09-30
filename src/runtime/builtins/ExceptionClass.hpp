#ifndef ARIA_EXCEPTION_CLASS_HPP
#define ARIA_EXCEPTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Exception bootstrap 类的自持单元:**唯一公有口是 make_class**。方法体与方法表仍住
    // ExceptionClass.cpp 匿名命名空间。本类是用户异常基类(可继承定义自己的异常类型);全部
    // bootstrap 类经 builtins_ 裸名曝光(键写点在 AriaVM 侧,不经 make_class)。VM 只在 ctor
    // 构造临界区内调用(创建免守卫,入寄存器即根)。
    class ExceptionClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 ExceptionClass 格由
        // 调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_EXCEPTION_CLASS_HPP
