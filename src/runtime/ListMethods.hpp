#ifndef ARIA_LIST_METHODS_HPP
#define ARIA_LIST_METHODS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // list 方法面(List bootstrap 类表条目)的注册入口,住 runtime 层(方法面是 VM 侧语言面,
    // object 层保持纯表示;对标 Builtins.hpp 的 register_builtins,后续各类型方法批同型:
    // StringMethods/MapMethods/...)。实现与方法体在 ListMethods.cpp,VM 只在
    // bootstrap_list_class 编排调用。须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    void register_list_methods(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_LIST_METHODS_HPP
