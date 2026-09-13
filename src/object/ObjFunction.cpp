#include "object/ObjFunction.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjFunction::ObjFunction(GC& gc, ObjModule* module, ObjString* name, const u8 arity) :
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

    ObjFunction* new_function(GC& gc, ObjModule* module, const StringView name, const u8 arity) {
        // 便捷重载:name 串由工厂自己创建,自守跨下方 new_object(守「自己创建的」);
        // module 仍须调用方根化,契约同 ObjString* 版。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_function(gc, module, name_str, arity);
    }

} // namespace aria
