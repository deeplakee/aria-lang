#include "object/ObjClosure.hpp"

#include "memory/GC.hpp"
#include "object/ObjClass.hpp" // mark_object(defining_class_) 须完整类型(派生自 Object 的转换)
#include "object/ObjFunction.hpp"
#include "object/ObjUpvalue.hpp"

namespace aria {

    ObjClosure::ObjClosure(GC& gc, ObjFunction* function) :
        Object{ObjType::CLOSURE}, function_{function}, upvalues_{&gc}, defining_class_{nullptr} {
        ASSERT(function != nullptr, "ObjClosure: function must not be null");
    }

    void ObjClosure::add_upvalue(ObjUpvalue* uv) {
        ASSERT(uv != nullptr, "ObjClosure::add_upvalue: upvalue must not be null");
        upvalues_.push(uv);
    }

    ObjString* ObjClosure::name() const noexcept {
        // function_ 恒非空(ctor ASSERT),函数名恒非空(ObjFunction ctor ASSERT),故指针恒非空。
        return function_->name();
    }

    void ObjClosure::trace(GC& gc) const noexcept {
        gc.mark_object(function_);
        for (ObjUpvalue* uv: upvalues_) { // const Array<T*> 遍历出的元素是 T*(指针本身 const,不传染 pointee)
            gc.mark_object(uv);
        }
        gc.mark_object(defining_class_); // 非方法闭包为 nullptr,mark_object 容 nullptr;类静态表方法闭包经此级联标所属类
    }

    String ObjClosure::debug_repr() const {
        // function_ 恒非空(ctor ASSERT):直取其名渲染 `<fn name>`,与 ObjFunction 同文案
        //(不经 function_->to_string() 虚委托,调试路径保持一跳纯 C++ 访问器)。
        return std::format("<fn {}>", function_->name()->view());
    }

    ObjClosure* new_closure(GC& gc, ObjFunction* function) {
        // 守卫纪律见 Object.hpp;调用方须自行根化 function_(通常已入常量池);建成须立即发布进根。
        return gc.new_object<ObjClosure>(gc, function);
    }

} // namespace aria
