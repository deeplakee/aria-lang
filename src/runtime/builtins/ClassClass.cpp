#include "runtime/builtins/ClassClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* ClassClass::make_class(GC& gc, ObjClass* super) {
        // Class 元类:类值对象的类面载体,方法表全空(成员一律沿链继承 Object 根)。
        return new_class(gc, "Class", super);
    }

} // namespace aria
