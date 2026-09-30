#ifndef ARIA_OBJECT_CLASS_HPP
#define ARIA_OBJECT_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Object 根类(Object bootstrap 类)的自持单元:**唯一公有口是 make_class**(根类,super 为空)。
    // 方法体与方法表仍住 ObjectClass.cpp 匿名命名空间。VM 只在 bootstrap_object_class 编排调用;
    // 须在 ctor 构造临界区内调用(创建免守卫,入寄存器即根)。
    class ObjectClass {
    public:
        // 唯一构造口:建根类并装载方法面,以裸指针返回;发布进寄存器 ObjectClass 格由调用点承担。
        static ObjClass* make_class(GC& gc);
    };

} // namespace aria

#endif // ARIA_OBJECT_CLASS_HPP
