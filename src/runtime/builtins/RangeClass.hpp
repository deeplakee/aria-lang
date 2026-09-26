#ifndef ARIA_RANGE_CLASS_HPP
#define ARIA_RANGE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // range 方法面(Range bootstrap 类表条目)的宿主类:唯一成员是注册口,方法体与方法表仍住
    // RangeClass.cpp 匿名命名空间(方法面是 VM 侧语言面,object 层保持纯表示)。VM 只在
    // bootstrap_range_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入表即根)。
    class RangeClass {
    public:
        static void register_methods(GC& gc, ObjClass* klass);
    };

} // namespace aria

#endif // ARIA_RANGE_CLASS_HPP
