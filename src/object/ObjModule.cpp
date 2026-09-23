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
        // dir_ / name_ 恒非空(见 new_module 的 cwd 兜底与合成顶层构造);构造期拦截非法 null。
        ASSERT(dir != nullptr, "ObjModule dir must not be null");
        ASSERT(name != nullptr, "ObjModule name must not be null");
    }

    String ObjModule::abs_path() const {
        // dir_ 指针恒非空,内容可空:空串表「无目录锚点」,resolve_module 据此判空(裸名分支跳过
        // 空根、相对分支直接返 nullopt),避免拿 "." 碰运气锚到错目录。
        if (dir_->view().empty()) {
            return {};
        }
        const StringView dir = dir_->view();
        // name_ 恒非空,仅判内容空:空串表合成顶层模块无文件名,仅返目录。
        if (name_->view().empty()) {
            return String{dir};
        }
        return std::format("{}/{}{}", dir, name_->view(), kAriaExtension);
    }

    String ObjModule::format_location(const u32 line) const {
        // 合成模块(名以 '<' 开头 -- aria.hpp 约定该前缀标记 VM 合成实体)的 abs_path 会拼出
        // dir_/<script>.aria 这类伪路径;无目录锚点(abs_path 空)同样无文件身份。两者一并退化为名。
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

    String ObjModule::debug_repr() const {
        // name_ 恒非空(内容可空)。
        return std::format("<module {}>", name_->view());
    }

    Opt<Value> ObjModule::load_field(AriaVM& vm, ObjString* name) {
        // 模块成员 = 模块全局绑定:键与 DEF/LOAD_GLOBAL 同源(intern),查表即命中判定,纯查询
        // 无分配。命中值原样直读 -- 顶层函数值为闭包且恒非方法,故不绑定 this(调用经 CALL_METHOD
        // 时槽 0 留模块值,闭包不读)。nil 绑定与「无此成员」由 find 的空态区分。miss 文案同基类默认。
        if (const auto entry = globals_.find(Value::from_obj(name))) {
            return entry->value;
        }
        return vm.fail(ErrorCode::UndefinedProperty, "{} has no member '{}'", this->debug_repr(), name->view());
    }

    bool ObjModule::store_field(AriaVM& vm, ObjString* name, const Value value) {
        // 模块成员只读:越模块写会隐式创建他人未声明全局,违「赋值不隐式创建」;暴露可变状态
        // 走模块自己的函数。
        return vm.fail(ErrorCode::TypeMismatch, "module members are read-only");
    }

    ObjModule* new_module(GC& gc, ObjString* name, ObjString* dir) {
        // 守卫纪律见 Object.hpp;调用方须自行根化 name 与 dir。
        return gc.new_object<ObjModule>(gc, name, dir);
    }

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
        // name_str / dir_str 均本函数内部新建并自守;dir 原样 intern(空串即空串锚点,不替
        // 调用方做 cwd 退化)。
        auto       guard    = gc.make_guard();
        const auto name_str = new_string(gc, name);
        guard.push(name_str);
        const auto dir_str = new_string(gc, dir);
        guard.push(dir_str);
        return new_module(gc, name_str, dir_str);
    }

} // namespace aria
