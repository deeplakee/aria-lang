#include "runtime/builtins/ModuleClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* ModuleClass::make_class(GC& gc, ObjClass* super) { return new_class(gc, "Module", super); }

} // namespace aria
