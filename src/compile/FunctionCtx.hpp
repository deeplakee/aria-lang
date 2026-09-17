#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 单函数编译上下文：持当前函数的局部栈 / 作用域深度 / 循环上下文栈 / 捕获描述表 / 指向外层上下文。
// 进 fun/lambda 压一层、退出弹一层（CodeGen 经 ModuleCtx::current_fn_ctx_ 持当前上下文指针）。
//
// 本类只负责「登记」（局部 / 作用域 / 循环 break-continue 的栈管理 / upvalue 捕获描述，并返回
// 弹出数等数据）；「发射」（emit_op / 跳转回填 / 错误）仍由 CodeGen 负责。
//
// 局部栈（clox 风格）：locals_[0] = 哑元（slot 0 = callee，隐含不命名）；
//   1..A = 形参（caller 压栈，编译期 add_local 登记）；
//   A+1.. = 函数体局部（值填槽：值先压栈，add_local 登记槽位 = 当前栈高 = 值所在位置，
//     登记即初始化；无 store/pop、不预占槽）。
//
// 循环上下文栈随函数走：进新函数即得空 loop_stack_，故 break/continue 不会跨函数绑定到
// 外层循环（函数边界天然隔离循环上下文）。

#include "compile/FnKind.hpp"
#include "object/ObjFunction.hpp"
#include "type.hpp"

namespace aria {

    // 单函数捕获 upvalue 上限(LOAD/STORE_UPVALUE 的 idx 为 u8,可寻址 0..kMaxUpvalues,
    // 故容量 = 上限 + 1:第 256 个捕获 idx=255 仍合法,第 257 个越界被拒)。与 CodeGen 的
    // kMax* 同一「上限位置」语义家族;因登记侧 add_upvalue 与报错文案共用而定义于本头。
    constexpr u32 kMaxUpvalues = kU8OperandMax;

    // this 局部名:实例方法帧槽 0 的具名局部。this 是关键字(Lexer 产出 This 关键字 token),
    // 不可能与用户标识符撞名,可安全作局部登记名参与 find_local / resolve_upvalue(嵌套函数
    // 引用 this 即沿 ctx 链捕获其槽 0,M4 机制零改动复用)。
    constexpr StringView kThisName = "this";

    // 局部变量条目（slot 0 = 哑元 callee）。简单聚合，默认 is_captured=false。
    struct Local {
        String name;
        u32    depth       = 0;
        bool   is_captured = false;
    };

    // 循环上下文（break / continue 回填）。简单聚合：
    //   - loop_scope_depth：循环体所在 scope 深度（break/continue 弹局部至此）。
    //   - back_target：循环头（条件求值点,while/for-in 的 L_start、for 的 L_cond）——所有循环公有的
    //     唯一编译期已知跳转目标：回边恒跳此，后向 continue 亦跳此。
    //   - continue_fwd_patches：前向 continue 占位偏移（-> L_incr），nullopt = 本循环无前向通道
    //     （continue 后向跳 back_target）；仅 for 带 incr 在入栈前 emplace 打开。
    //   - exit_fwd_patches：前向退出占位偏移（循环头条件假跳 JUMP_FALSE + 各 break 的 JUMP），
    //     循环收尾 emit_loop_backedge_and_exits 统一回填 -> L_end。
    struct LoopCtx {
        u32              loop_scope_depth     = 0;
        u32              back_target          = 0;
        Opt<List<usize>> continue_fwd_patches = std::nullopt;
        List<usize>      exit_fwd_patches;
    };

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // 单构造：enclosing 为 nullptr 即入口 <main> 上下文（enclosing_=nullptr = entry），
        // 否则嵌套函数上下文（指向外层）。kind 为函数种类（默认普通函数；实例方法时槽 0 =
        // 具名局部 this，见 FnKind 注）。
        explicit FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, FnKind kind);

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

        // 当前函数局部查表，返最近一个同名局部 slot（最内层）；未命中返 nullopt。
        [[nodiscard]]
        Opt<u16> find_local(StringView name) const;

        // --- upvalue 登记（M4 闭包）---
        // 登记一条捕获描述，返回 upvalue 索引：同 (is_local,index) 已登记即复用其下标（同一局部
        // 被多处引用只占一个 upvalue）；未登记则追加。
        //
        // upvalue 索引为 u8（LOAD/STORE_UPVALUE 操作数域，可寻址 0..kMaxUpvalues），追加将越出
        // 索引域（size > kMaxUpvalues，即 256 条已满、新条目 idx 将为 256）返 nullopt，由 CodeGen
        // fail(TooManyUpvalues)。纯登记，不发射。
        //
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
        FnKind            kind_;
        List<Local>       locals_;
        u32               scope_depth_;
        Stack<LoopCtx>    loop_stack_;
        List<UpvalueDesc> upvalues_; // M4 闭包捕获描述表（按下标即 upvalue 索引），经 add_upvalue 逐条登记
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
