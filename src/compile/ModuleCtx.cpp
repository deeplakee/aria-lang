#include "compile/ModuleCtx.hpp"

#include "common.hpp"
#include "compile/FunctionCtx.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    ModuleCtx::ModuleCtx(ObjModule* module) : module_{module} {
        ASSERT(module->entry() != nullptr, "module entry must be set before construction");
        current_fn_ctx_ = new FunctionCtx(module->entry(), nullptr, FnKind::Function);
    }

    // 沿 enclosing_ 链逐个 delete（成功 / 出错两路径的游标位置分析见头注）；先存 next 再 delete
    // （delete 后不可再读 ctx）。
    ModuleCtx::~ModuleCtx() {
        auto ctx = current_fn_ctx_;
        while (ctx != nullptr) {
            const auto next = ctx->enclosing_;
            delete ctx;
            ctx = next;
        }
    }

    bool ModuleCtx::is_global_scope() const noexcept {
        return current_fn_ctx_->enclosing_ == nullptr && current_fn_ctx_->scope_depth_ == 0;
    }

} // namespace aria
