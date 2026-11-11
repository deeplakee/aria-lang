#ifndef ARIA_FUNCTION_CLASS_HPP
#define ARIA_FUNCTION_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Function 单例类自持单元:原生与 aria 函数共用的类面载体,唯一公有口是 make_class(super = Object 根);
    // 方法表全空,成员一律沿链继承根,住同名 .cpp。
    class FunctionClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_FUNCTION_CLASS_HPP
