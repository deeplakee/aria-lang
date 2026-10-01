#include "object/ObjFunction.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjModule.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjFunction::ObjFunction(GC& gc, ObjModule* module, ObjString* name, const u8 arity, const u8 min_arity,
                             const bool is_varargs) :
        Object{ObjType::FUNCTION}, unit_{&gc}, module_{module}, name_{name}, arity_{arity}, min_arity_{min_arity},
        is_varargs_{is_varargs}, upvalue_descs_{&gc} {
        ASSERT(module != nullptr, "module must not be null (every function belongs to a module)");
        ASSERT(name != nullptr, "function name must not be null (entry=<main>, lambda=<anonymous>)");
        ASSERT(min_arity <= arity, "min_arity must not exceed arity");
    }

    void ObjFunction::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(module_); // 回指所属模块,与 module->entry_ 成环,mark-sweep 三色标记破环
        unit_.trace(gc);
    }

    String ObjFunction::debug_repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<fn {}>", name_->view());
    }

    ObjFunction* new_function(GC& gc, ObjModule* module, ObjString* name, const u8 arity, const u8 min_arity,
                              const bool is_varargs) {
        return gc.new_object<ObjFunction>(gc, module, name, arity, min_arity, is_varargs);
    }

    ObjFunction* new_function(GC& gc, ObjModule* module, const StringView name, const u8 arity, const u8 min_arity,
                              const bool is_varargs) {
        // name 串由工厂自己创建,自守跨下方 new_object;module 仍须调用方根化。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_function(gc, module, name_str, arity, min_arity, is_varargs);
    }

} // namespace aria
