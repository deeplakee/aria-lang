#ifndef ARIA_STRING_CLASS_HPP
#define ARIA_STRING_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // string 方法面(String bootstrap 类)自持单元:公有面 = make_class,方法体与方法表住同名 .cpp。
    class StringClass {
    public:
        // 唯一构造口,建类挂 super。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_STRING_CLASS_HPP
