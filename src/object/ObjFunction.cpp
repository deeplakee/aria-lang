#include "object/ObjFunction.hpp"

#include <format>

#include "aria.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjFnProto.hpp"
#include "object/ObjString.hpp"
#include "object/ObjUpvalue.hpp"
#include "runtime/AriaExeContext.hpp"

namespace aria {

    ObjFunction::ObjFunction(GC& gc, ObjFnProto* proto) :
        Object{ObjType::FUNCTION}, payload_{std::in_place_type<AriaData>, gc, proto}, defining_class_{nullptr} {
        ASSERT(proto != nullptr, "function prototype must not be null");
    }

    ObjFunction::ObjFunction(ObjString* name, const NativeFn fn) :
        Object{ObjType::FUNCTION}, payload_{std::in_place_type<NativeData>, name, fn}, defining_class_{nullptr} {
        ASSERT(name != nullptr, "function name must not be null (use kAnonymousName for anonymous)");
        ASSERT(fn != nullptr, "function pointer must not be null");
    }

    ObjString* ObjFunction::name() const noexcept {
        return is_native() ? std::get<NativeData>(payload_).name : std::get<AriaData>(payload_).proto->name();
    }

    NativeFn ObjFunction::fn() const noexcept {
        ASSERT(is_native(), "function must be native");
        return std::get<NativeData>(payload_).fn;
    }

    ObjFnProto* ObjFunction::proto() const noexcept {
        ASSERT(!is_native(), "function must have an aria payload");
        return std::get<AriaData>(payload_).proto;
    }

    const Array<ObjUpvalue*>& ObjFunction::upvalues() const noexcept {
        ASSERT(!is_native(), "function must have an aria payload");
        return std::get<AriaData>(payload_).upvalues;
    }

    void ObjFunction::add_upvalue(ObjUpvalue* upvalue) {
        ASSERT(!is_native(), "function must have an aria payload");
        ASSERT(upvalue != nullptr, "upvalue must not be null");
        std::get<AriaData>(payload_).upvalues.push(upvalue);
    }

    void ObjFunction::trace(GC& gc) const noexcept {
        gc.mark_object(defining_class_);
        if (is_native()) {
            gc.mark_object(std::get<NativeData>(payload_).name);
        } else {
            const auto& data = std::get<AriaData>(payload_);
            gc.mark_object(data.proto);
            for (const auto upvalue: data.upvalues) {
                gc.mark_object(upvalue);
            }
        }
    }

    String ObjFunction::repr() const { return std::format("<fn {}>", name()->view()); }

    Opt<Value> ObjFunction::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.function_class();
        if (const auto value = klass->find_field(name)) {
            return value;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    ObjFunction* new_function(GC& gc, ObjFnProto* proto) { return gc.new_object<ObjFunction>(gc, proto); }

    ObjFunction* new_function(GC& gc, ObjString* name, const NativeFn fn) {
        return gc.new_object<ObjFunction>(name, fn);
    }

    ObjFunction* new_function(GC& gc, const NativeFn fn) { return new_function(gc, kAnonymousName, fn); }

    ObjFunction* new_function(GC& gc, const StringView name, const NativeFn fn) {
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_function(gc, name_str, fn);
    }

} // namespace aria
