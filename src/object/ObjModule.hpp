#ifndef ARIA_OBJ_MODULE_HPP
#define ARIA_OBJ_MODULE_HPP

#include "common.hpp"
#include "object/Object.hpp"
#include "value/AriaHashTable.hpp"

namespace aria {

    class GC;
    class ObjString;
    class ObjFnProto;

    // 文件模块用文件 stem 与目录，原生标准库用逻辑全名与空目录。两者指针非空。
    class ObjModule final : public Object {
    public:
        ObjModule(GC& gc, ObjString* name, ObjString* dir);
        ~ObjModule() override = default; // globals_ 持 GC* 级联自释放;entry_/name_/dir_ 是 GC 对象,不归本类释放

        [[nodiscard]]
        ObjString* name() const noexcept {
            return name_;
        }

        [[nodiscard]]
        ObjString* dir() const noexcept {
            return dir_;
        }

        // 无目录锚点时返回空串；名称为空时返回目录。
        [[nodiscard]]
        String abs_path() const;

        // 文件模块用绝对路径，合成模块或无目录锚点时用名称；不触发 GC。
        [[nodiscard]]
        String format_location(u32 line) const;

        [[nodiscard]]
        AriaHashTable& globals() noexcept {
            return globals_;
        }

        // nullptr 表示无模块体或模块体尚未编译。
        [[nodiscard]]
        ObjFnProto* entry() const noexcept {
            return entry_;
        }

        void set_entry(ObjFnProto* entry) noexcept { entry_ = entry; }

        void trace(GC& gc) const noexcept override;

        [[nodiscard]]
        usize size() const noexcept override {
            return sizeof(ObjModule);
        }

        [[nodiscard]]
        String repr() const override;

        [[nodiscard]]
        Opt<Value> load_field_from_class(AriaExeContext& ctx, ObjString* name) override;

        // 普通成员只查模块 globals，不回退类链，不绑定。
        [[nodiscard]]
        Opt<Value> load_field(AriaExeContext& ctx, ObjString* name) override;

        [[nodiscard]]
        Opt<Value> load_field_bound(AriaExeContext& ctx, ObjString* name) override;

        // 跨模块写入会绕过全局声明，因此成员只读。
        [[nodiscard]]
        bool store_field(AriaExeContext& ctx, ObjString* name, Value value) override;

    private:
        ObjString*    name_; // 模块名(intern;指针恒非空,内容可空)
        ObjString*    dir_;  // 所在目录(intern;指针恒非空,内容可空)
        ObjFnProto*   entry_;
        AriaHashTable globals_; // 模块级绑定表(惰性分配)
    };

    // 调用方须根化 name 和 dir，dir 指针须非空。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name, ObjString* dir);

    // 省略 dir 时取 cwd；显式传空串表示无目录锚点。
    [[nodiscard]]
    ObjModule* new_module(GC& gc, ObjString* name);

    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name);

    [[nodiscard]]
    ObjModule* new_module(GC& gc, StringView name, StringView dir);

} // namespace aria

#endif // ARIA_OBJ_MODULE_HPP
