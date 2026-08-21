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
        // name 经 intern 驻留池是 weak root,new_object 顶部 maybe_collect 可能回收未被根持有的串,
        // 故先入临时根(与 new_function / new_module 同理)。fn 为标量,无需入根。
        auto guard = gc.make_guard(name);
        return gc.new_object<ObjNativeFn>(name, fn);
    }

} // namespace aria
