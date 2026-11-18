#include "runtime/builtins/FunctionClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* FunctionClass::make_class(GC& gc, ObjClass* super) { return new_class(gc, "Function", super); }

} // namespace aria
