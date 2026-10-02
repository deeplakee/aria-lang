#ifndef ARIA_MAP_CLASS_HPP
#define ARIA_MAP_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // map 方法面(Map bootstrap 类)自持单元:唯一公有口是 make_class,方法体与方法表住同名 .cpp。
    class MapClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_MAP_CLASS_HPP
