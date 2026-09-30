#ifndef ARIA_ITERATOR_CLASS_HPP
#define ARIA_ITERATOR_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // 迭代器方法面(Iterator bootstrap 类)的自持单元:**唯一公有口是 make_class**。方法体是
    // ObjIterator 引擎缝虚函数的薄壳 -- has_next()/next(vm) 各一个虚调用,不按源分派(各源差异
    // 收在 src/object/iterator/ 的子类里);方法体与方法表仍住 IteratorClass.cpp 匿名命名空间。
    // VM 只在 bootstrap_iterator_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入寄存器
    // 即根)。
    class IteratorClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 IteratorClass 格由
        // 调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_ITERATOR_CLASS_HPP
