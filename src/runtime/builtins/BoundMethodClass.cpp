#include "runtime/builtins/BoundMethodClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* BoundMethodClass::make_class(GC& gc, ObjClass* super) {
        // BoundMethod 类:绑定方法值的类面载体,方法表全空(成员一律沿链继承 Object 根)。
        return new_class(gc, "BoundMethod", super);
    }

} // namespace aria
