#include "object/ObjModule.hpp"

#include <format>

#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"
#include "util/fs.hpp"

namespace aria {

    ObjModule::ObjModule(GC& gc, ObjString* name, ObjString* root) :
        Object{ObjType::MODULE}, name_{name}, root_{root}, entry_{nullptr}, globals_{&gc},
        state_{ModuleState::Loading} {
        // root_ 恒非空(new_module 默认 cwd 兜底):构造期拦截非法 null,与 SourceLoc 同模式。
        // name_ 可为 nullptr(合成顶层 <script>),不在此约束。
        ASSERT(root != nullptr, "ObjModule root must not be null");
    }

    void ObjModule::trace(GC& gc) const noexcept {
        // mark_object 容 nullptr(GC.cpp:18),entry_ 可为 nullptr 直接标(root_ 恒非空,见 new_module)。
        gc.mark_object(name_);
        gc.mark_object(root_);
        gc.mark_object(entry_);
        globals_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value)
    }

    String ObjModule::to_string() const {
        if (name_ == nullptr) {
            return "<module>";
        }
        return std::format("<module {}>", name_->view());
    }

    String ObjModule::abs_path() const {
        // root_ 指针恒非空(构造期 ASSERT,无合法指针空态 -- 不像 SourceLoc 有默认构造空态),无需判 null。
        // 但内容可空:new_module 2 参重载在 cwd 不可用时以空串兜底。空串表「无源根锚点」(合成 <script>
        // / REPL / cwd 失败):resolve_module 据此判空,裸名分支跳过空根、相对分支 current_abs 为空直接返
        // nullopt,避免拿 "." 碰运气锚到错目录。
        if (root_->view().empty()) {
            return {};
        }
        const StringView root = root_->view();
        if (name_ == nullptr || name_->view().empty()) {
            return String{root}; // 合成顶层模块无文件名,仅返源根
        }
        return std::format("{}/{}.aria", root, name_->view());
    }

    ObjModule* new_module(GC& gc, ObjString* name, ObjString* root) {
        // 工厂不替调用方守卫入参:本重载只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name 与 root(跨 new_object 顶 maybe_collect)。
        return gc.new_object<ObjModule>(gc, name, root);
    }

    ObjModule* new_module(GC& gc, ObjString* name) {
        // 调用方须保证 name 在本调用期间已根化:下方 new_string(cwd) 与最终 new_object 均 GC。
        // root_str 是本函数内部新建、调用方看不到,故自行守卫跨下方 new_object(工厂守「自己创建的」)。
        // cwd 不可用时以空串兜底(不 fatal,见头注释):下游空值守卫拒绝 cwd 锚定。cwd 串经 intern 驻留。
        const auto root_str = new_string(gc, fs::current_dir().value_or(""));
        const auto guard    = gc.make_guard(root_str);
        return new_module(gc, name, root_str);
    }

    ObjModule* new_module(GC& gc, const StringView name) {
        // name_str 是本函数内部新建,工厂自行守卫跨下方 new_module 内部的 new_string(cwd) 与 new_object
        // (工厂守「自己创建的」)。调用方传 StringView,无需手动建串根化。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_module(gc, name_str);
    }

    ObjModule* new_module(GC& gc, const StringView name, const StringView root) {
        // name_str 与 root_str 均本函数内部新建,工厂自行守卫跨下方 new_module(GC&, ObjString*, ObjString*)
        //   内部的 new_object 顶 maybe_collect(工厂守「自己创建的」)。调用方传 StringView,无需手动建串根化。
        //   root 串原样 intern 调用方给的串:空串即空串锚点(不替调用方做 cwd 退化,需退化用 2 参 StringView 重载)。
        auto       guard    = gc.make_guard();
        const auto name_str = new_string(gc, name);
        guard.push(name_str);
        const auto root_str = new_string(gc, root);
        guard.push(root_str);
        return new_module(gc, name_str, root_str);
    }

} // namespace aria
