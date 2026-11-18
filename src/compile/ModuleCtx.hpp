#ifndef ARIA_MODULECTX_HPP
#define ARIA_MODULECTX_HPP

// 拥有当前函数上下文链；编译失败时沿链释放尚未还原的子上下文。

#include "type.hpp"

namespace aria {

    class ObjModule;
    class FunctionCtx;

    class ModuleCtx {
    public:
        ModuleCtx() = delete; // 必须绑定 module 构造

        // module 须已设置入口原型。
        explicit ModuleCtx(ObjModule* module);

        ~ModuleCtx();

        ModuleCtx(const ModuleCtx&)            = delete;
        ModuleCtx& operator=(const ModuleCtx&) = delete;
        ModuleCtx(ModuleCtx&&)                 = delete;
        ModuleCtx& operator=(ModuleCtx&&)      = delete;

        bool declare_global(const StringView name) { return defined_globals_.insert(name).second; }

        [[nodiscard]]
        bool is_global_scope() const noexcept;

        ObjModule* module_;

        FunctionCtx* current_fn_ctx_;

    private:
        HashSet<StringView> defined_globals_; // 已登记顶层全局名（键借视图，存活覆盖编译期）
    };

} // namespace aria

#endif // ARIA_MODULECTX_HPP
