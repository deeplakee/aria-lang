#ifndef ARIA_MAP_BUILTINS_HPP
#define ARIA_MAP_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // map 方法面(Map bootstrap 类表条目)的注册入口,住 runtime/builtins/(方法面是 VM 侧语言面,
    // object 层保持纯表示;对标 Builtins.hpp 的 register_builtin_functions,后续各类型方法批同型:
    // StringBuiltins/RangeBuiltins/...)。实现与方法体在 MapBuiltins.cpp,VM 只在
    // bootstrap_map_class 编排调用。须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    void register_map_builtins(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_MAP_BUILTINS_HPP
