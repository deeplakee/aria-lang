#include "object/ObjNativeFn.hpp"

#include <format>

#include "aria.hpp"
#include "memory/GC.hpp"
#include "object/ObjBoundMethod.hpp"
#include "object/ObjClass.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaExeContext.hpp"
#include "value/ObjBridge.hpp"

namespace aria {

    ObjNativeFn::ObjNativeFn(ObjString* name, const NativeFn fn) :
        Object{ObjType::NATIVE_FN}, name_{name}, fn_{fn}, defining_class_{nullptr} {
        ASSERT(fn != nullptr, "function pointer must not be null");
        // name_ 恒非空:匿名用 kAnonymousName。
        ASSERT(name != nullptr, "function name must not be null (use kAnonymousName for anonymous)");
    }

    void ObjNativeFn::trace(GC& gc) const noexcept {
        gc.mark_object(name_);
        gc.mark_object(defining_class_);
    }

    String ObjNativeFn::repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<fn {}>", name_->view());
    }

    Opt<Value> ObjNativeFn::load_field_from_class(AriaExeContext& ctx, ObjString* name) {
        const auto klass = ctx.native_fn_class();
        if (const auto v = klass->find_field(name)) {
            return v;
        }
        return ctx.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", klass->repr(), name->view());
    }

    Opt<Value> ObjNativeFn::load_field(AriaExeContext& ctx, ObjString* name) {
        return load_field_from_class(ctx, name);
    }

    Opt<Value> ObjNativeFn::load_field_bound(AriaExeContext& ctx, ObjString* name) {
        const auto hit = load_field(ctx, name);
        if (!hit) {
            return std::nullopt;
        }
        const auto member = *hit;
        if (!aria::is_method(member)) {
            return member;
        }
        return Value::from_obj(new_bound_method(ctx.gc(), member, this));
    }

    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, const NativeFn fn) {
        return gc.new_object<ObjNativeFn>(name, fn);
    }

    ObjNativeFn* new_native_fn(GC& gc, const NativeFn fn) {
        // 匿名重载:委托 StringView 名重载,驻留与守卫由其内部完成。
        return new_native_fn(gc, kAnonymousName, fn);
    }

    ObjNativeFn* new_native_fn(GC& gc, const StringView name, const NativeFn fn) {
        // name_str 由本函数内部 intern 并自守跨下方 new_object;调用方传文本即可。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_native_fn(gc, name_str, fn);
    }

} // namespace aria
