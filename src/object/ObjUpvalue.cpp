#include "object/ObjUpvalue.hpp"

#include "memory/GC.hpp"

namespace aria {

    void ObjUpvalue::trace(GC& gc) const noexcept {
        // open 标栈槽内值 / closed 标 closed_:value_slot() 两态同址语义,统一走它。
        gc.mark_value(*value_slot());
    }

    String ObjUpvalue::debug_repr() const { return "<upvalue>"; }

    ObjUpvalue* new_upvalue(GC& gc, Value* slot) { return gc.new_object<ObjUpvalue>(slot); }

} // namespace aria
