#ifndef ARIA_RANGE_BUILTINS_HPP
#define ARIA_RANGE_BUILTINS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // range 方法面(Range bootstrap 类表条目)的注册入口,住 runtime/builtins/(方法面是 VM 侧语言面,
    // object 层保持纯表示;对标 Builtins.hpp 的 register_builtins,集合类型方法批同型)。实现与
    // 方法体在 RangeBuiltins.cpp,VM 只在 bootstrap_range_class 编排调用。须在 ctor 构造临界区内
    // 调用(创建免守卫,入表即根)。
    void register_range_builtins(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_RANGE_BUILTINS_HPP
