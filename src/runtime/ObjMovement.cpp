#include "runtime/ObjMovement.hpp"

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"

namespace aria {

    // 帧成员 mark_object 的基类转换需完整类型,故住 .cpp。基类状态(值栈/帧/开指/寄存器)经
    // 友元直取;previous_ 为本类成员。
    void ObjMovement::trace(GC& gc) const noexcept {
        for (auto p = buf_.data(); p < top_; ++p) {
            gc.mark_value(*p); // mark_value 对非对象 Value no-op,栈槽含 int/f64/bool/nil 安全
        }
        for (const auto& frame: frames_.span()) {
            gc.mark_object(frame.closure); // trace 级联标 function/upvalues;容 nullptr
            gc.mark_object(frame.module);
        }
        // 开链节点可能仅被本链引用(闭包已死),须单独标(mark 幂等,双标无害)。
        for (auto upvalue = open_upvalues_; upvalue != nullptr; upvalue = upvalue->next_open()) {
            gc.mark_object(upvalue);
        }
        if (const auto& pending = pending_error()) {
            gc.mark_value(*pending);
        }
        gc.mark_object(previous_); // resume 链链上成员经灰栈级联,容 nullptr
    }

    ObjMovement* new_movement(GC& gc, AriaVM& host) { return gc.new_object<ObjMovement>(&gc, &host); }

} // namespace aria
