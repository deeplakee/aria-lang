#include "runtime/builtins/NativeFnClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* NativeFnClass::make_class(GC& gc, ObjClass* super) {
        // NativeFn 类:原生函数值的类面载体,方法表全空(成员一律沿链继承 Object 根)。
        return new_class(gc, "NativeFn", super);
    }

} // namespace aria
