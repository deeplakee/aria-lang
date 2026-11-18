#include "runtime/builtins/BoundMethodClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* BoundMethodClass::make_class(GC& gc, ObjClass* super) { return new_class(gc, "BoundMethod", super); }

} // namespace aria
