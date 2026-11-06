#ifndef ARIA_BOUND_METHOD_CLASS_HPP
#define ARIA_BOUND_METHOD_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // BoundMethod 单例类自持单元:绑定方法值的类面载体,唯一公有口是 make_class(super = Object 根);
    // 方法表全空,成员一律沿链继承根,住同名 .cpp。
    class BoundMethodClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_BOUND_METHOD_CLASS_HPP
