#include "object/ObjFunction.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjFunction::ObjFunction(GC& gc, ObjModule* module, ObjString* name, u8 arity) :
        Object{ObjType::FUNCTION}, unit_{&gc}, module_{module}, name_{name}, arity_{arity} {
        ASSERT(module != nullptr, "ObjFunction: module must not be null (every function belongs to a module)");
    }

    void ObjFunction::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(module_); // 非空;mark_object 容 nullptr 仅防御。回指所属模块
        unit_.trace(gc);
    }

    String ObjFunction::to_string() const {
        if (name_ == nullptr) {
            return "<script>";
        }
        return std::format("<fn {}>", name_->view());
    }

    ObjFunction* new_function(GC& gc, ObjModule* module, ObjString* name, u8 arity) {
        // module 与 name 皆可能未被根持有:module 调用方可能尚未入 VM 模块表,name 经 intern
        // 是 weak root。new_object 顶部 maybe_collect 可能回收二者,故先入临时根。
        auto guard = gc.make_guard(module);
        guard.push(name);
        return gc.new_object<ObjFunction>(gc, module, name, arity);
    }

} // namespace aria
