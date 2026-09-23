#ifndef ARIA_ITERATOR_BUILTINS_HPP
#define ARIA_ITERATOR_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // 迭代器方法面(Iterator bootstrap 类表条目:has_next/next)的注册入口,住 runtime/builtins/
    //(与 Builtins.hpp 各 register_* 入口同型)。方法体是 ObjIterator 引擎缝虚函数的薄壳 --
    // has_next()/next(vm) 各一个虚调用,不按源分派(各源差异收在 src/object/iterator/ 的子类里)。
    // 须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    void register_iterator_builtins(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_ITERATOR_BUILTINS_HPP
