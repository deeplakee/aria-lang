#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 单函数编译上下文：持当前函数的局部栈 / 作用域深度 / 循环上下文栈 / 指向外层上下文。
// 进 fun/lambda 压一层、退出弹一层（CodeGen 经 ModuleCtx::current_fn_ctx_ 持当前上下文指针）。
// 本类只负责「登记」（局部 / 作用域 / 循环 break-continue 的栈管理，并返回弹出数等数据）；
// 「发射」（emit_op / 跳转回填 / 错误）仍由 CodeGen 负责。
//
// 局部栈（clox 风格）：locals_[0] = 哑元（slot 0 = callee，隐含不命名）；
//   1..A = 形参（caller 压栈，编译期 add_local 登记后 mark_initialized）；
//   A+1.. = 函数体局部（CodeGen::declare_local 仅登记并标「定义但未初始化」，不发指令；
//     调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized，无 store/pop 预占）。
//
// 循环上下文栈随函数走：进新函数即得空 loop_stack_，故 break/continue 不会跨函数绑定到
// 外层循环（函数边界天然隔离循环上下文）。

#include "type.hpp"

namespace aria {

    class ObjFunction;

    // 局部变量条目（slot 0 = 哑元 callee）。简单聚合，默认 is_captured/is_initialized=false。
    // is_initialized：定义但未初始化（declare_local 置 false）；初始化器求值 / 无初始化器发
    // LOAD_NIL 后由调用方 mark_initialized 置 true。读取未初始化局部 -> CodeGen 报 UninitializedVariable。
    struct Local {
        String name;
        u32    depth          = 0;
        bool   is_captured    = false;
        bool   is_initialized = false;
    };

    // 循环上下文（break / continue 回填）。简单聚合。
    struct LoopCtx {
        u32         loop_scope_depth     = 0;            // 循环体所在 scope 深度（break/continue 弹局部至此）
        Opt<u32>    continue_back_target = std::nullopt; // 后向 continue 目标（while / for-in / for 无 incr）
        List<usize> continue_fwd_patches;                // 前向 continue 回填（for 有 incr -> L_incr）
        List<usize> break_fwd_patches;                   // 待回填的 JUMP 占位偏移
    };

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // 入口 <script> 上下文（enclosing_=nullptr = entry）。
        explicit FunctionCtx(ObjFunction& fn);

        // 嵌套函数上下文（enclosing_ 指向外层）。
        FunctionCtx(FunctionCtx& enclosing, ObjFunction& fn);

        FunctionCtx(const FunctionCtx&)                = delete;
        FunctionCtx& operator=(const FunctionCtx&)     = delete;
        FunctionCtx(FunctionCtx&&) noexcept            = delete;
        FunctionCtx& operator=(FunctionCtx&&) noexcept = delete;

        // --- 局部 / 作用域 ---
        // 压 Local{name, scope_depth_}，返回 slot = locals_.size()-1（纯登记，不发射、不查重）。
        u16 add_local(StringView name);

        // 当前作用域是否已定义同名局部（外层同名允许 shadow）。
        [[nodiscard]]
        bool is_defined_in_scope(StringView name) const;

        // 标记局部已初始化（初始化器求值 / 无初始化器发 LOAD_NIL / 赋值后调用）。
        void mark_initialized(u16 slot);

        // 局部是否已初始化（读取前由 CodeGen 检查，未初始化 -> fail UninitializedVariable）。
        [[nodiscard]]
        bool is_initialized(u16 slot) const;

        // 当前函数局部查表，返最近一个同名局部 slot（最内层）；未命中返 nullopt。
        [[nodiscard]]
        Opt<u16> find_local(StringView name) const;

        // 进入块作用域。
        void begin_scope();

        // 退出块作用域：--scope_depth_ 后弹比新 scope_depth_ 更深的局部（即原 scope 的局部），返回弹出数（调用方 emit
        // POP_N）。
        u32 end_scope_pop_count();

        // 弹深度 > target_depth 的局部（更内层作用域的局部），返回弹出数（不改变 scope_depth_）。
        // end_scope 退出作用域、break/continue 跳出循环均用此（不限于控制流）。
        u32 pop_locals_deeper_than(u32 target_depth);

        // --- 成员（公开，CodeGen 直接访问 fn_/locals_/loop_stack_ 等）---
        // enclosing_ 所有权由调用方局部 UPtr 持有（CodeGen::compile 入口 / compile_function 子）：
        // 父函数编译期长于子函数（栈帧包住），故 enclosing_ 裸指针在子生命期内稳定。enclosing_==nullptr 即入口。
        // 当前发射的 CodeUnit 由 CodeGen 经 cur_cu() 派生（= &fn_->unit()，随 ModuleCtx 游标），不缓存于本类。
        FunctionCtx* enclosing_;
        ObjFunction*     fn_;
        List<Local>      locals_;
        u32              scope_depth_;
        List<LoopCtx>    loop_stack_;
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
