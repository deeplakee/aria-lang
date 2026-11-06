#ifndef ARIA_CLASS_CLASS_HPP
#define ARIA_CLASS_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Class 元类单例自持单元:类值(const value)作为对象所属的类,唯一公有口是 make_class
    //(super = Object 根);方法体与表住同名 .cpp。
    class ClassClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_CLASS_CLASS_HPP
