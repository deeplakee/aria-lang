#include "object/ObjModule.hpp"

#include <format>

#include "aria.hpp"
#include "memory/GC.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjString.hpp"
#include "util/fs.hpp"

namespace aria {

    ObjModule::ObjModule(GC& gc, ObjString* name, ObjString* dir) :
        Object{ObjType::MODULE}, name_{name}, dir_{dir}, entry_{nullptr}, globals_{&gc} {
        // dir_ 恒非空(new_module 默认 cwd 兜底)、name_ 恒非空(合成顶层 <script> / REPL 等均以非空
        // intern 串构造):构造期拦截非法 null,与 SourceLoc 同模式。
        ASSERT(dir != nullptr, "ObjModule dir must not be null");
        ASSERT(name != nullptr, "ObjModule name must not be null");
    }

    void ObjModule::trace(GC& gc) const noexcept {
        // entry_ 可为 nullptr(未编译/目录包占位),mark_object 容 nullptr。
        gc.mark_object(name_);
        gc.mark_object(dir_);
        gc.mark_object(entry_);
        globals_.trace(gc); // 遍历占用槽 mark_value(key) + mark_value(value)
    }

    String ObjModule::debug_repr() const {
        // name_ 恒非空(ctor ASSERT),内容可空(合成顶层)但指针非空。
        return std::format("<module {}>", name_->view());
    }

    String ObjModule::abs_path() const {
        // dir_ 指针恒非空(构造期 ASSERT),但内容可空:空串表「无目录锚点」(cwd 不可用兜底),
        // resolve_module 据此判空 -- 裸名分支跳过空根、相对分支直接返 nullopt,
        // 避免拿 "." 碰运气锚到错目录。
        if (dir_->view().empty()) {
            return {};
        }
        const StringView dir = dir_->view();
        // name_ 恒非空(ctor ASSERT),仅判内容空:空串表合成顶层模块无文件名,仅返目录。
        if (name_->view().empty()) {
            return String{dir};
        }
        return std::format("{}/{}{}", dir, name_->view(), kAriaExtension);
    }

    ObjModule* new_module(GC& gc, ObjString* name, ObjString* dir) {
        // 工厂不替调用方守卫入参:只做一次 new_object、无内部新建对象,调用方须在调用前自行
        // 根化 name 与 dir(跨 new_object 顶 maybe_collect)。
        return gc.new_object<ObjModule>(gc, name, dir);
    }

    ObjModule* new_module(GC& gc, ObjString* name) {
        // 调用方须根化 name(下方 new_string(cwd) 与最终 new_object 均 GC);dir_str 本函数
        // 内部新建、调用方看不到,自行守卫(工厂守「自己创建的」)。cwd 不可用时以空串兜底
        //(见头注释)。
        const auto dir_str = new_string(gc, fs::current_dir().value_or(""));
        const auto guard   = gc.make_guard(dir_str);
        return new_module(gc, name, dir_str);
    }

    ObjModule* new_module(GC& gc, const StringView name) {
        // name_str 本函数内部新建,工厂自行守卫跨下方 new_module 内部的 new_string(cwd) 与
        // new_object(工厂守「自己创建的」)。调用方传 StringView,无需手动建串根化。
        const auto name_str = new_string(gc, name);
        const auto guard    = gc.make_guard(name_str);
        return new_module(gc, name_str);
    }

    ObjModule* new_module(GC& gc, const StringView name, const StringView dir) {
        // name_str 与 dir_str 均本函数内部新建,工厂自行守卫跨下方 new_object 顶 maybe_collect
        //(工厂守「自己创建的」)。dir 串原样 intern 调用方给的串:空串即空串锚点(不替调用方
        // 做 cwd 退化,需退化用 2 参 StringView 重载)。
        auto       guard    = gc.make_guard();
        const auto name_str = new_string(gc, name);
        guard.push(name_str);
        const auto dir_str = new_string(gc, dir);
        guard.push(dir_str);
        return new_module(gc, name_str, dir_str);
    }

} // namespace aria
