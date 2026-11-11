#include "object/ObjBoundMethod.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFunction.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    ObjBoundMethod::ObjBoundMethod(ObjFunction* method, const Value receiver) :
        Object{ObjType::BOUND_METHOD}, method_{method}, receiver_{receiver} {
        ASSERT(method != nullptr, "method must not be null");
    }

    ObjString* ObjBoundMethod::name() const noexcept { return method_->name(); }

    void ObjBoundMethod::trace(GC& gc) const noexcept {
        gc.mark_object(method_);
        gc.mark_value(receiver_);
    }

    bool ObjBoundMethod::equals(const Object* other) const noexcept {
        // 绑定语义即「同一函数对象 + 同一接收者」,比较不触发 GC。
        ASSERT(other != nullptr, "null object pointer");
        if (this == other) {
            return true;
        }
        if (const auto rhs = other->try_as<ObjBoundMethod>()) {
            return method_ == rhs->method_ && value_identical(receiver_, rhs->receiver_);
        }
        return false;
    }

    String ObjBoundMethod::repr() const { return std::format("<bound method {}>", name()->view()); }

    Opt<Value> ObjBoundMethod::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.bound_method_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, const Value receiver) {
        return gc.new_object<ObjBoundMethod>(method, receiver);
    }

    ObjBoundMethod* new_bound_method(GC& gc, ObjFunction* method, Object* receiver) {
        return new_bound_method(gc, method, Value::from_obj(receiver));
    }

} // namespace aria
