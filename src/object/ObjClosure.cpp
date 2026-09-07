#include "object/ObjClosure.hpp"

#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    ObjClosure::ObjClosure(GC& gc, ObjFunction* function) :
        Object{ObjType::CLOSURE}, function_{function}, upvalues_{&gc} {
        ASSERT(function != nullptr, "ObjClosure: function must not be null");
    }

    void ObjClosure::add_upvalue(ObjUpvalue* uv) {
        ASSERT(uv != nullptr, "ObjClosure::add_upvalue: upvalue must not be null");
        upvalues_.push(uv);
    }

    void ObjClosure::trace(GC& gc) const noexcept {
        gc.mark_object(function_);
        for (ObjUpvalue* uv: upvalues_) { // const Array<T*> 遍历出的元素是 T*(指针本身 const,不传染 pointee)
            gc.mark_object(uv);
        }
    }

    String ObjClosure::to_string() const {
        // function_ 恒非空(ctor ASSERT):渲染 `<fn name>`,与 ObjFunction 同文案。
        return function_->to_string();
    }

    ObjClosure* new_closure(GC& gc, ObjFunction* function) {
        // 工厂不替调用方守卫入参:本工厂只做一次 new_object、无内部新建对象,调用方须在调用前
        // 自行根化 function_(通常已入常量池)。返回对象白色无根,须立即发布进根。
        return gc.new_object<ObjClosure>(gc, function);
    }

} // namespace aria
