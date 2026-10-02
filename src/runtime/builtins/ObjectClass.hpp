#ifndef ARIA_OBJECT_CLASS_HPP
#define ARIA_OBJECT_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Object 根类(Object bootstrap 类)自持单元:唯一公有口是 make_class(根类,super 为空),
    // 方法体与方法表住同名 .cpp。
    class ObjectClass {
    public:
        // 唯一构造口:建根类并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc);
    };

} // namespace aria

#endif // ARIA_OBJECT_CLASS_HPP
