#ifndef ARIA_OBJECT_CLASS_HPP
#define ARIA_OBJECT_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Object 根类方法面(Object bootstrap 类表条目)的宿主类:唯一成员是注册口,方法体与方法表
    // 仍住 ObjectClass.cpp 匿名命名空间。VM 只在 bootstrap_object_class 编排调用;须在 ctor
    // 构造临界区内调用(创建免守卫,入表即根)。
    class ObjectClass {
    public:
        static void register_methods(GC& gc, ObjClass* klass);
    };

} // namespace aria

#endif // ARIA_OBJECT_CLASS_HPP
