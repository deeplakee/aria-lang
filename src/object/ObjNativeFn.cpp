#include "object/ObjNativeFn.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjNativeFn::ObjNativeFn(ObjString* name, NativeFn fn) : Object{ObjType::NATIVE_FN}, name_{name}, fn_{fn} {
        ASSERT(fn != nullptr, "ObjNativeFn: function pointer must not be null");
    }

    void ObjNativeFn::trace(GC& gc) const noexcept {
        gc.mark_object(name_); // fn_ 是 C++ 指针,非 GC 对象;name_ 可能为 nullptr,mark_object 仅防御
    }

    String ObjNativeFn::to_string() const {
        if (name_ == nullptr) {
            return "<fn>";
        }
        return std::format("<fn {}>", name_->view());
    }

    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, NativeFn fn) {
        // 工厂不替调用方守卫入参:本工厂只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name(跨 new_object 顶 maybe_collect)。fn 为标量,无需入根。
        return gc.new_object<ObjNativeFn>(name, fn);
    }

} // namespace aria
