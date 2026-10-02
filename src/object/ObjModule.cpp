#include "object/ObjModule.hpp"

#include <format>

#include "aria.hpp"
#include "error/ErrorCode.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"
#include "runtime/AriaVM.hpp"
#include "util/fs.hpp"

namespace aria {

    ObjModule::ObjModule(GC& gc, ObjString* name, ObjString* dir) :
        Object{ObjType::MODULE}, name_{name}, dir_{dir}, entry_{nullptr}, globals_{&gc} {
        ASSERT(dir != nullptr, "module directory must not be null");
        ASSERT(name != nullptr, "module name must not be null");
    }

    String ObjModule::abs_path() const {
        // 内容空串表「无目录锚点」:resolve_module 判空拒绝,不拿 "." 碰运气锚到错目录。
        if (dir_->view().empty()) {
            return {};
        }
        const StringView dir = dir_->view();
        if (name_->view().empty()) {
            return String{dir};
        }
        return std::format("{}/{}{}", dir, name_->view(), kAriaExtension);
    }

    String ObjModule::format_location(const u32 line) const {
        const auto name = name_->view();
        const auto path = abs_path();
        const auto base = (name.starts_with('<') || path.empty()) ? name : StringView{path};
        return std::format("{}:{}", base, line);
    }

    void ObjModule::trace(GC& gc) const noexcept {
        // entry_ 可为 nullptr(未编译/目录包占位),mark_object 容 nullptr。
        gc.mark_object(name_);
        gc.mark_object(dir_);
        gc.mark_object(entry_);
        globals_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value)
    }

    String ObjModule::debug_repr() const { return std::format("<module {}>", name_->view()); }

    Opt<Value> ObjModule::load_field(AriaVM& vm, ObjString* name) {
        if (const auto entry = globals_.find(Value::from_obj(name))) {
            return entry->value;
        }
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    bool ObjModule::store_field(AriaVM& vm, ObjString* name, const Value value) {
        return vm.fail(ErrorCode::TypeMismatch, "type {} does not support field assignment", type_name());
    }

    ObjModule* new_module(GC& gc, ObjString* name, ObjString* dir) { return gc.new_object<ObjModule>(gc, name, dir); }

    ObjModule* new_module(GC& gc, ObjString* name) {
        // 调用方须根化 name;dir_str 本函数内部新建并自守(工厂守「自己创建的」)。
        const auto dir_str = new_string(gc, fs::current_dir().value_or(""));
        const auto guard   = gc.make_guard(dir_str);
        return new_module(gc, name, dir_str);
    }

    ObjModule* new_module(GC& gc, const StringView name) {
        // name_str 本函数内部新建并自守跨下方 new_module;调用方传 StringView 即可。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_module(gc, name_str);
    }

    ObjModule* new_module(GC& gc, const StringView name, const StringView dir) {
        // name_str / dir_str 均本函数内部新建并自守;dir 原样 intern(空串锚点,不替调用方做 cwd 退化)。
        auto       guard    = gc.make_guard();
        const auto name_str = new_string(gc, name);
        guard.push(name_str);
        const auto dir_str = new_string(gc, dir);
        guard.push(dir_str);
        return new_module(gc, name_str, dir_str);
    }

} // namespace aria
