#include "runtime/ObjMovement.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaVM.hpp"

namespace aria {

    // 帧成员 mark_object 的基类转换需完整类型,故住 .cpp。基类状态(值栈/帧/开指/寄存器与
    // previous_ 链)一律经友元直取。
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

    Opt<Value> ObjMovement::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.coroutine_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    Opt<Value> ObjMovement::load_field(AriaExeContext& ctx, ObjString* name) {
        return load_field_from_class(ctx, name);
    }

    ObjMovement* new_movement(GC& gc, AriaVM& host) { return gc.new_object<ObjMovement>(&gc, &host); }

} // namespace aria
