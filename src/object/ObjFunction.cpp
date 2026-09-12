#include "object/ObjFunction.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjFunction::ObjFunction(GC& gc, ObjModule* module, ObjString* name, u8 arity) :
        Object{ObjType::FUNCTION}, unit_{&gc}, module_{module}, name_{name}, arity_{arity}, upvalue_descs_{&gc} {
        ASSERT(module != nullptr, "ObjFunction: module must not be null (every function belongs to a module)");
        ASSERT(name != nullptr, "ObjFunction: name must not be null (entry=<main>, lambda=<anonymous>)");
    }

    void ObjFunction::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(module_); // 回指所属模块,与 module->entry_ 成环,mark-sweep 三色标记破环
        unit_.trace(gc);
    }

    String ObjFunction::debug_repr() const {
        // name_ 恒非空(ctor ASSERT),统一 `<fn name>`。
        return std::format("<fn {}>", name_->view());
    }

    ObjFunction* new_function(GC& gc, ObjModule* module, ObjString* name, const u8 arity) {
        // 工厂不替调用方守卫入参:module 与 name 皆是 weak root,调用方须在调用前自行根化
        //(契约见头注释)。调用方裸持 fresh 对象直接传入是 bug,需 make_guard。
        return gc.new_object<ObjFunction>(gc, module, name, arity);
    }

} // namespace aria
