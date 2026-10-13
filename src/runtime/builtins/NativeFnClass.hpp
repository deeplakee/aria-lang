#ifndef ARIA_NATIVE_FN_CLASS_HPP
#define ARIA_NATIVE_FN_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // NativeFn 单例类自持单元:原生函数值的类面载体,唯一公有口是 make_class(super = Object 根);
    // 方法表全空,成员一律沿链继承根,住同名 .cpp。
    class NativeFnClass {
    public:
        // 唯一构造口:建类并装载方法面;返回白色对象,发布进根由调用点承担。
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_NATIVE_FN_CLASS_HPP
