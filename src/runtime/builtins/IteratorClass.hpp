#ifndef ARIA_ITERATOR_CLASS_HPP
#define ARIA_ITERATOR_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Iterator bootstrap 类：内建迭代器方法面的宿主，方法体与其方法表住同名 .cpp。
    class IteratorClass {
    public:
        // 唯一构造口:建类(挂 super)并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_ITERATOR_CLASS_HPP
