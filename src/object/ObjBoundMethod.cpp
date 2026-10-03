#include "object/ObjBoundMethod.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjNativeFn.hpp"
#include "value/ObjBridge.hpp"

namespace aria {

    ObjBoundMethod::ObjBoundMethod(const Value method, const Value receiver) :
        Object{ObjType::BOUND_METHOD}, method_{method}, receiver_{receiver} {
        // method_ 须为可调用值(闭包或原生函数);判定收口 is_callable_value。
        ASSERT(is_callable_value(method), "method must be a callable (closure or native fn)");
    }

    ObjString* ObjBoundMethod::name() const noexcept {
        // 非虚读取:闭包取 fn 名、原生取 name_(两者皆 intern 驻留恒非空,指针随宿主对象保命)。
        const auto m = method_.as_obj();
        if (m->type() == ObjType::CLOSURE) {
            return m->as<ObjClosure>()->name();
        }
        return m->as<ObjNativeFn>()->name();
    }

    void ObjBoundMethod::trace(GC& gc) const noexcept {
        gc.mark_value(method_);   // 方法值装箱任意对象(闭包/原生),mark_value 分派
        gc.mark_value(receiver_); // receiver 装箱任意值,mark_value 分派
    }

    bool ObjBoundMethod::equals(const Object* other) const noexcept {
        // 绑定语义即「同一实现 + 同一接收者」,皆按身份(value_identical)比较,无 GC 分配,GC-pure 契约保持。
        ASSERT(other != nullptr, "null object pointer");
        if (this == other) {
            return true;
        }
        if (const auto rhs = other->try_as<ObjBoundMethod>()) {
            return value_identical(method_, rhs->method_) && value_identical(receiver_, rhs->receiver_);
        }
        return false;
    }

    String ObjBoundMethod::debug_repr() const { return std::format("<bound method {}>", name()->view()); }

    ObjBoundMethod* new_bound_method(GC& gc, const Value method, const Value receiver) {
        return gc.new_object<ObjBoundMethod>(method, receiver);
    }

} // namespace aria
