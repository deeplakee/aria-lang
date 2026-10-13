#include "runtime/builtins/ClosureClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* ClosureClass::make_class(GC& gc, ObjClass* super) {
        // Closure 类:闭包值的类面载体,方法表全空(成员一律沿链继承 Object 根)。
        return new_class(gc, "Closure", super);
    }

} // namespace aria
