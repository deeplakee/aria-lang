#ifndef ARIA_MODULE_CLASS_HPP
#define ARIA_MODULE_CLASS_HPP

#include "common.hpp"

namespace aria {

    class GC;
    class ObjClass;

    // Module 单例类自持单元:import 装载的模块对象的类面载体,唯一公有口是 make_class
    //(super = Object 根);方法表全空,成员一律沿链继承根,住同名 .cpp。
    class ModuleClass {
    public:
        static ObjClass* make_class(GC& gc, ObjClass* super);
    };

} // namespace aria

#endif // ARIA_MODULE_CLASS_HPP
