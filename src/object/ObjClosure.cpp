#include "object/ObjClosure.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    ObjClosure::ObjClosure(GC& gc, ObjFnProto* proto) :
        Object{ObjType::CLOSURE}, proto_{proto}, upvalues_{&gc}, defining_class_{nullptr} {
        ASSERT(proto != nullptr, "function prototype must not be null");
    }

    ObjString* ObjClosure::name() const noexcept {
        // proto_ 恒非空(ctor ASSERT),函数名恒非空,故指针恒非空。
        return proto_->name();
    }

    void ObjClosure::add_upvalue(ObjUpvalue* uv) {
        ASSERT(uv != nullptr, "upvalue must not be null");
        upvalues_.push(uv);
    }

    void ObjClosure::trace(GC& gc) const noexcept {
        gc.mark_object(proto_);
        for (ObjUpvalue* uv: upvalues_) {
            gc.mark_object(uv);
        }
        gc.mark_object(defining_class_); // 非方法闭包为 nullptr,容 nullptr;方法闭包经此级联标所属类
    }

    String ObjClosure::repr() const {
        // proto_ 恒非空(ctor ASSERT):直取名渲染,不经虚委托(调试路径保持一跳)。
        return std::format("<fn {}>", proto_->name()->view());
    }

    Opt<Value> ObjClosure::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.closure_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    ObjClosure* new_closure(GC& gc, ObjFnProto* function_proto) {
        return gc.new_object<ObjClosure>(gc, function_proto);
    }

} // namespace aria
