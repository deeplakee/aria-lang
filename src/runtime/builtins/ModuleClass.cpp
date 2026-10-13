#include "runtime/builtins/ModuleClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* ModuleClass::make_class(GC& gc, ObjClass* super) {
        // Module 类:import 装载的模块对象的类面载体,方法表全空(成员一律沿链继承 Object 根;
        // 模块自身 globals 是各自的值态,不属本类)。
        return new_class(gc, "Module", super);
    }

} // namespace aria
