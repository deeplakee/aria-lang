#ifndef ARIA_RANGE_CLASS_HPP
#define ARIA_RANGE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // range 方法面(Range bootstrap 类表条目)的注册入口,住 runtime/builtins/(方法面是 VM 侧语言面,
    // object 层保持纯表示;注册入口命名规律见 Builtins.hpp)。实现与方法体在 RangeClass.cpp,
    // VM 只在 bootstrap_range_class 编排调用。须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    void register_range_builtins(GC& gc, ObjClass* klass);

} // namespace aria

#endif // ARIA_RANGE_CLASS_HPP
