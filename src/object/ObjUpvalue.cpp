#include "object/ObjUpvalue.hpp"

#include "memory/GC.hpp"

namespace aria {

    void ObjUpvalue::trace(GC& gc) const noexcept { gc.mark_value(*value_slot()); }

    String ObjUpvalue::repr() const { return "<upvalue>"; }

    ObjUpvalue* new_upvalue(GC& gc, Value* slot) { return gc.new_object<ObjUpvalue>(slot); }

} // namespace aria
