#include "compile/ModuleCtx.hpp"

#include "common.hpp"
#include "compile/FunctionCtx.hpp"
#include "object/ObjFunction.hpp"
#include "object/ObjModule.hpp"

namespace aria {

    // ============================================================
    // 构造 / 析构
    // ============================================================

    // 调用方须先 module.set_entry(entry)；构造期 ASSERT entry 非空，就 m.entry() new 一个入口 fn 上下文
    // （enclosing_==nullptr = entry）赋值给 current_fn_ctx_--它既是入口所有者也是当前游标（初始 = 入口）。
    ModuleCtx::ModuleCtx(ObjModule* module) : module_{module} {
        ASSERT(module->entry() != nullptr, "ModuleCtx 构造前须 set_entry 入口函数");
        current_fn_ctx_ = new FunctionCtx(module->entry());
    }

    // 沿 enclosing_ 链（current_fn_ctx_ → 父 → ... → entry）逐个 delete，无论游标在哪儿都对：
    //   - 成功路径：compile_function 已在还原游标后手动 delete 各子上下文，析构时游标 = entry，仅删 entry。
    //   - 出错路径：compile_function 不还原游标、不 delete 子，直接 return（出错即停，见 CodeGen）；析构从
    //     游标（最深层未释放子）走链释放整条活动链 + entry。
    // 先存 next 再 delete（delete 后不可再读 ctx）。
    ModuleCtx::~ModuleCtx() {
        auto ctx = current_fn_ctx_;
        while (ctx != nullptr) {
            const auto next = ctx->enclosing_;
            delete ctx;
            ctx = next;
        }
    }

    // 当前函数为入口（enclosing_==nullptr）且在第 0 层作用域 -> 模块顶层：var/fun/import 别名
    // 定义为模块全局（DEF_GLOBAL / IMPORT），否则为局部。游标 current_fn_ctx_ 即「当前编译到的函数」。
    bool ModuleCtx::is_global_scope() const noexcept {
        return current_fn_ctx_->enclosing_ == nullptr && current_fn_ctx_->scope_depth_ == 0;
    }

} // namespace aria
