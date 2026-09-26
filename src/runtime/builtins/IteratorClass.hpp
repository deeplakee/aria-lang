#ifndef ARIA_ITERATOR_CLASS_HPP
#define ARIA_ITERATOR_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // 迭代器方法面(Iterator bootstrap 类表条目)的宿主类:唯一成员是注册口,方法体与方法表仍住
    // IteratorClass.cpp 匿名命名空间。方法体是 ObjIterator 引擎缝虚函数的薄壳 -- has_next()/next(vm)
    // 各一个虚调用,不按源分派(各源差异收在 src/object/iterator/ 的子类里)。VM 只在
    // bootstrap_iterator_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    class IteratorClass {
    public:
        static void register_methods(GC& gc, ObjClass* klass);
    };

} // namespace aria

#endif // ARIA_ITERATOR_CLASS_HPP
