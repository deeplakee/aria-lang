#include "object/ObjClosure.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    ObjClosure::ObjClosure(GC& gc, ObjFunction* function) :
        Object{ObjType::CLOSURE}, function_{function}, upvalues_{&gc}, defining_class_{nullptr} {
        ASSERT(function != nullptr, "function must not be null");
    }

    ObjString* ObjClosure::name() const noexcept {
        // function_ 恒非空(ctor ASSERT),函数名恒非空,故指针恒非空。
        return function_->name();
    }

    void ObjClosure::add_upvalue(ObjUpvalue* uv) {
        ASSERT(uv != nullptr, "upvalue must not be null");
        upvalues_.push(uv);
    }

    void ObjClosure::trace(GC& gc) const noexcept {
        gc.mark_object(function_);
        for (ObjUpvalue* uv: upvalues_) {
            gc.mark_object(uv);
        }
        gc.mark_object(defining_class_); // 非方法闭包为 nullptr,容 nullptr;方法闭包经此级联标所属类
    }

    String ObjClosure::debug_repr() const {
        // function_ 恒非空(ctor ASSERT):直取名渲染,不经虚委托(调试路径保持一跳)。
        return std::format("<fn {}>", function_->name()->view());
    }

    ObjClosure* new_closure(GC& gc, ObjFunction* function) { return gc.new_object<ObjClosure>(gc, function); }

} // namespace aria
