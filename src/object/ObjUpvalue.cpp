#include "object/ObjUpvalue.hpp"

#include "memory/GC.hpp"

namespace aria {

    void ObjUpvalue::trace(GC& gc) const noexcept {
        // open 标栈槽内值 / closed 标 closed_:value_slot() 两态同址语义,统一走它。
        gc.mark_value(*value_slot());
    }

    String ObjUpvalue::debug_repr() const { return "<upvalue>"; }

    ObjUpvalue* new_upvalue(GC& gc, Value* slot) {
        // 入参是裸栈槽地址,无对象可守;本工厂只做一次 new_object、无内部新建对象。
        // 返回对象白色无根,调用方须立即链入 VM 开链(链上节点经 VM 根 tracer 保命)。
        return gc.new_object<ObjUpvalue>(slot);
    }

} // namespace aria
