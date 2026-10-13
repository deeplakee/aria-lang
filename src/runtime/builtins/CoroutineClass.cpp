#include "runtime/builtins/CoroutineClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* CoroutineClass::make_class(GC& gc, ObjClass* super) {
        // Coroutine 类:协程句柄的类面载体,方法表全空(成员一律沿链继承 Object 根)。
        return new_class(gc, "Coroutine", super);
    }

} // namespace aria
