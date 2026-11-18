#include "runtime/builtins/ClassClass.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"

namespace aria {

    ObjClass* ClassClass::make_class(GC& gc, ObjClass* super) { return new_class(gc, "Class", super); }

} // namespace aria
