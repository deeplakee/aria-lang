#ifndef ARIA_EXCEPTION_CLASS_HPP
#define ARIA_EXCEPTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Exception bootstrap 类自持单元:唯一公有口 make_class,方法体与方法表住同名 .cpp;
    // 用户异常基类(可继承定义自己的异常类型)。
    class ExceptionClass {
    public:
        // 唯一构造口,建类挂 super。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_EXCEPTION_CLASS_HPP
