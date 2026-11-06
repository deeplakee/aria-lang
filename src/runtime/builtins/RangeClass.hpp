#ifndef ARIA_RANGE_CLASS_HPP
#define ARIA_RANGE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // range 方法面(Range bootstrap 类)自持单元:唯一公有口是 make_class,方法体与方法表住同名 .cpp。
    class RangeClass {
    public:
        // 唯一构造口,建类挂 super。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_RANGE_CLASS_HPP
