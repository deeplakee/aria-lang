#ifndef ARIA_MODULECTX_HPP
#define ARIA_MODULECTX_HPP

// 模块编译上下文：模块句柄 + 当前函数上下文游标（current_fn_ctx_ 兼拥有入口 fn 上下文）+ 顶层全局名注册表。
// 一次性实例，用完即弃；出错时未还原的子上下文交 ~ModuleCtx 沿链释放。

#include "type.hpp"

namespace aria {

    class ObjModule;
    class ObjFunction;
    class FunctionCtx;

    class ModuleCtx {
    public:
        ModuleCtx() = delete; // 必须绑定 module 构造

        // 绑定模块句柄并新建入口 fn 上下文、就位游标；须先 set_entry（ASSERT）。
        explicit ModuleCtx(ObjModule* module);

        // 沿 enclosing_ 链逐个 delete，无论游标在哪都对。
        ~ModuleCtx();

        ModuleCtx(const ModuleCtx&)            = delete;
        ModuleCtx& operator=(const ModuleCtx&) = delete;
        ModuleCtx(ModuleCtx&&)                 = delete;
        ModuleCtx& operator=(ModuleCtx&&)      = delete;

        // 登记顶层全局名：true = 新登记，false = 已存在。
        bool declare_global(const StringView name) { return defined_globals_.insert(String{name}).second; }

        // 当前是否模块顶层作用域；定义于 .cpp（需 FunctionCtx 完整类型）。
        [[nodiscard]]
        bool is_global_scope() const noexcept;

        ObjModule* module_;

        // 当前函数上下文游标，兼拥有入口 fn 上下文。
        FunctionCtx* current_fn_ctx_;

    private:
        HashSet<String> defined_globals_; // 已登记顶层全局名
    };

} // namespace aria

#endif // ARIA_MODULECTX_HPP
