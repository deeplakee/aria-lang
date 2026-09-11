#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 单函数编译上下文：持当前函数的局部栈 / 作用域深度 / 循环上下文栈 / 捕获描述表 / 指向外层上下文。
// 进 fun/lambda 压一层、退出弹一层（CodeGen 经 ModuleCtx::current_fn_ctx_ 持当前上下文指针）。
// 本类只负责「登记」（局部 / 作用域 / 循环 break-continue 的栈管理 / upvalue 捕获描述，并返回
// 弹出数等数据）；「发射」（emit_op / 跳转回填 / 错误）仍由 CodeGen 负责。
//
// 局部栈（clox 风格）：locals_[0] = 哑元（slot 0 = callee，隐含不命名）；
//   1..A = 形参（caller 压栈，编译期 add_local 登记后 mark_initialized）；
//   A+1.. = 函数体局部（CodeGen::declare_local_or_fail 仅登记并标「定义但未初始化」，不发指令；
//     调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized，无 store/pop、不预占槽）。
//
// 循环上下文栈随函数走：进新函数即得空 loop_stack_，故 break/continue 不会跨函数绑定到
// 外层循环（函数边界天然隔离循环上下文）。

#include "object/ObjFunction.hpp"
#include "type.hpp"

namespace aria {

    // 单函数捕获 upvalue 上限（kU8OperandMax 的语义名，与 CodeGen 的 kMaxArity/kMaxConstants/
    // kMaxLocals 同一「上限位置」语义家族：值即对应操作数/索引位宽可表的最大值，越界判定统一用
    // > 比较--拒绝发生在「新条目的索引将越出操作数域」之时；因登记侧 FunctionCtx::add_upvalue 与
    // CodeGen 报错文案共用，定义于本头）。LOAD/STORE_UPVALUE 的 idx 为 u8（可寻址 0..kMaxUpvalues），
    // 故容量 = 上限 + 1：第 256 个捕获 idx=255 仍合法，第 257 个 idx=256 越界被拒。
    constexpr u32 kMaxUpvalues = kU8OperandMax;

    // 局部变量条目（slot 0 = 哑元 callee）。简单聚合，默认 is_captured/is_initialized=false。
    // is_initialized：定义但未初始化（declare_local_or_fail 置 false）；初始化器求值 / 无初始化器发
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

    // 工厂：构造仅指定 loop_scope_depth 的新循环上下文（continue_back_target=nullopt、
    // 两个 patch 列表空），集中收口「新 LoopCtx 全字段初始化」。调用方按循环类型在入栈前给
    // continue_back_target 赋值（while/for-in -> L_start、for 无 incr -> L_cond；for 有 incr
    // 留空走前向 continue_fwd_patches）。
    [[nodiscard]]
    inline LoopCtx make_loop_ctx(const u32 loop_scope_depth) {
        return {.loop_scope_depth     = loop_scope_depth,
                .continue_back_target = std::nullopt,
                .continue_fwd_patches = {},
                .break_fwd_patches    = {}};
    }

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // 入口 <main> 上下文（enclosing_=nullptr = entry）。
        explicit FunctionCtx(ObjFunction* fn);

        // 嵌套函数上下文（enclosing_ 指向外层）。
        FunctionCtx(FunctionCtx& enclosing, ObjFunction* fn);

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

        // --- upvalue 登记（M4 闭包）---
        // 登记一条捕获描述，返回 upvalue 索引：同 (is_local,index) 已登记即复用其下标（同一局部
        // 被多处引用只占一个 upvalue）；未登记则追加。upvalue 索引为 u8（LOAD/STORE_UPVALUE 操作
        // 数域，可寻址 0..kMaxUpvalues），追加将越出索引域（size > kMaxUpvalues，即 256 条已满、
        // 新条目 idx 将为 256）返 nullopt，由 CodeGen fail(TooManyUpvalues)。纯登记，不发射。
        // 表本体 upvalues_ 见下方成员区。
        Opt<u8> add_upvalue(UpvalueDesc desc);

        // 进入块作用域。
        void begin_scope();

        // 退出块作用域：--scope_depth_ 后弹出原 scope 的局部（depth > 新 scope_depth_，自最内层向外清）。
        // 真正从编译期 locals_ 移除--局部出作用域，后续语句不可再引用；break/continue 跳出循环不得走此
        // （跳转后的语句仍在作用域内可引用这些局部，须保留登记）。slot 0 哑元 depth=0 因 0 <= 任意
        // target_depth 恒在弹区界外。弹区清理指令（整区 POP_N + 含被捕获局部时一条批量 CLOSE_UPVALUE）
        // 由 CodeGen 先经 emit_pop_locals_to 发射（需在登记移除前做），本方法只管登记收尾。
        void end_scope();

        // --- 成员（公开，CodeGen 直接访问 fn_/locals_/loop_stack_/upvalues_ 等）---
        // enclosing_ 所有权：入口 fn 上下文由 ModuleCtx 构造期 new、~ModuleCtx 沿链 delete；子上下文由 compile_function
        // new（成功路径 delete、出错交 ~ModuleCtx 走链）。父函数编译期长于子函数（栈帧包住），故 enclosing_
        // 裸指针在子生命期内稳定。enclosing_==nullptr 即入口。 当前发射的 CodeUnit 由 CodeGen 经 cur_cu() 派生（=
        // &fn_->unit()，随 ModuleCtx 游标），不缓存于本类。
        FunctionCtx*      enclosing_;
        ObjFunction*      fn_;
        List<Local>       locals_;
        u32               scope_depth_;
        Stack<LoopCtx>    loop_stack_;
        List<UpvalueDesc> upvalues_; // M4 闭包捕获描述表（按下标即 upvalue 索引），经 add_upvalue 逐条登记
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
