#include "object/ObjNativeFn.hpp"

#include <format>

#include "aria.hpp"
#include "memory/GC.hpp"
#include "object/ObjString.hpp"

namespace aria {

    ObjNativeFn::ObjNativeFn(ObjString* name, NativeFn fn) : Object{ObjType::NATIVE_FN}, name_{name}, fn_{fn} {
        ASSERT(fn != nullptr, "ObjNativeFn: function pointer must not be null");
        // name_ 恒非空:无具名需求者用 kAnonymousName("<anonymous>"),与 ObjFunction 的 lambda 命名一致。
        ASSERT(name != nullptr, "ObjNativeFn: name must not be null (use kAnonymousName for anonymous)");
    }

    void ObjNativeFn::trace(GC& gc) const noexcept {
        gc.mark_object(name_); // fn_ 是 C++ 指针,非 GC 对象,唯一 GC 子节点即 name_
    }

    String ObjNativeFn::debug_repr() const {
        // name_ 恒非空(ctor ASSERT);匿名原生函数 name_ = kAnonymousName,渲染 `<fn <anonymous>>`。
        return std::format("<fn {}>", name_->view());
    }

    ObjNativeFn* new_native_fn(GC& gc, ObjString* name, const NativeFn fn) {
        // 工厂不替调用方守卫入参:本工厂只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name(跨 new_object 顶 maybe_collect)。fn 为标量,无需入根。
        return gc.new_object<ObjNativeFn>(name, fn);
    }

    ObjNativeFn* new_native_fn(GC& gc, const NativeFn fn) {
        // 匿名重载:name_str 是本函数内部新建,工厂自行守卫跨下方 new_object(「每方守自己创建的」)。
        // 调用方传 fn 即可,无需手动建串根化。委托显式名重载。
        const auto name_str = new_string(gc, kAnonymousName);
        const auto guard    = gc.make_guard(name_str);
        return new_native_fn(gc, name_str, fn);
    }

    ObjNativeFn* new_native_fn(GC& gc, const StringView name, const NativeFn fn) {
        // StringView 名重载:name_str 经 intern 由本函数内部新建,工厂自行守卫跨下方 new_object
        //(「每方守自己创建的」)。调用方传文本即可,无需手动建串根化。委托显式名重载。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_native_fn(gc, name_str, fn);
    }

} // namespace aria
