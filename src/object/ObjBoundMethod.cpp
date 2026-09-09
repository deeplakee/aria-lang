#include "object/ObjBoundMethod.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjClosure.hpp"
#include "object/ObjFunction.hpp"

namespace aria {

    ObjBoundMethod::ObjBoundMethod(ObjClosure* method, Value receiver) :
        Object{ObjType::BOUND_METHOD}, method_{method}, receiver_{receiver} {
        // method_ 恒非空(ctor ASSERT):无闭包无可绑定。
        ASSERT(method != nullptr, "ObjBoundMethod: method must not be null");
    }

    void ObjBoundMethod::trace(GC& gc) const noexcept {
        gc.mark_object(method_);
        gc.mark_value(receiver_); // receiver 装箱任意值,mark_value 分派
    }

    String ObjBoundMethod::to_string() const {
        // method_ 恒非空(ctor ASSERT):渲染方法名,与 <fn m> 渲染族同源(闭包 -> fn 名)。
        return std::format("<bound method {}>", method_->function()->name()->view());
    }

    ObjBoundMethod* new_bound_method(GC& gc, ObjClosure* method, Value receiver) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 method 与 receiver 中的对象。
        return gc.new_object<ObjBoundMethod>(method, receiver);
    }

} // namespace aria
