#ifndef ARIA_RANGE_CLASS_HPP
#define ARIA_RANGE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // range 方法面(Range bootstrap 类)的自持单元:**唯一公有口是 make_class**。方法体与方法表
    // 仍住 RangeClass.cpp 匿名命名空间(方法面是 VM 侧语言面,object 层保持纯表示)。VM 只在
    // bootstrap_range_class 编排调用;须在 ctor 构造临界区内调用(创建免守卫,入寄存器即根)。
    class RangeClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面,以裸指针返回;发布进寄存器 RangeClass 格由调用
        // 点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_RANGE_CLASS_HPP
