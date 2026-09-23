#include "object/ObjNativeFn.hpp"

#include <format>

#include "aria.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjNativeFn::ObjNativeFn(ObjString* name, NativeFn fn) : Object{ObjType::NATIVE_FN}, name_{name}, fn_{fn} {
        ASSERT(fn != nullptr, "ObjNativeFn: function pointer must not be null");
        // name_ 恒非空:匿名用 kAnonymousName。
        ASSERT(name != nullptr, "ObjNativeFn: name must not be null (use kAnonymousName for anonymous)");
    }

    void ObjNativeFn::trace(GC& gc) const noexcept {
        gc.mark_object(name_); // fn_ 是 C++ 指针非 GC 对象,唯一 GC 子节点即 name_
    }

    String ObjNativeFn::debug_repr() const {
        // name_ 恒非空(ctor ASSERT)。
        return std::format("<fn {}>", name_->view());
    }

    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, const NativeFn fn) {
        // 守卫纪律见 Object.hpp;调用方须自行根化 name(fn 为标量)。
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
