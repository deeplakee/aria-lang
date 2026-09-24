#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 单函数编译上下文：持当前函数的局部栈 / 作用域深度 / 循环上下文栈 / 捕获描述表 / 指向外层上下文。
// 本类只负责「登记」（局部 / 作用域 / 循环栈 / upvalue 捕获描述，并返回弹出数等数据）；「发射」
// （emit_op / 跳转回填 / 错误）仍由 CodeGen 负责。
// 局部栈（clox 风格）：locals_[0] = 哑元（slot 0 = callee）；1..A = 形参；A+1.. = 体局部（值填槽：
// 值先压栈，add_local 登记槽位 = 当前栈高 = 值所在位置，登记即初始化；无 store/pop、不预占槽）。
// 循环上下文栈随函数走：进新函数即得空 loop_stack_，break/continue 不会跨函数绑定外层循环。

#include "compile/FnKind.hpp"
#include "object/ObjFunction.hpp"
#include "type.hpp"

namespace aria {

    // 单函数捕获 upvalue 上限(LOAD/STORE_UPVALUE 的 idx 为 u8,可寻址 0..kMaxUpvalues,故容量 =
    // 上限 + 1):与 CodeGen 的 kMax* 同一「上限位置」语义家族,因登记侧 add_upvalue 与报错文案共用而定于此。
    constexpr u32 kMaxUpvalues = kU8OperandMax;

    // this 局部名:实例方法帧槽 0 的具名局部。this 是关键字,不可能与用户标识符撞名,可安全作局部
    // 登记名参与 find_local / resolve_upvalue(嵌套引用 this 即沿 ctx 链捕获其槽 0)。
    constexpr StringView kThisName = "this";

    // 局部变量条目（slot 0 = 哑元 callee）。简单聚合，默认 is_captured=false。
    struct Local {
        String name;
        u32    depth       = 0;
        bool   is_captured = false;
    };

    // 循环上下文（break / continue 回填）。简单聚合：
    //   - loop_scope_depth：循环体所在 scope 深度（break/continue 弹局部至此）。
    //   - back_target：循环头（条件求值点）--回边恒跳此，后向 continue 亦跳此。
    //   - continue_fwd_patches：前向 continue 占位偏移（-> L_incr），nullopt = 无前向通道（continue
    //     后向跳 back_target）；仅 for 带 incr 在入栈前 emplace 打开。
    //   - exit_fwd_patches：前向退出占位（循环头条件假跳 + 各 break 的 JUMP），收尾统一回填 -> L_end。
    struct LoopCtx {
        u32            loop_scope_depth     = 0;
        u32            back_target          = 0;
        Opt<List<u32>> continue_fwd_patches = std::nullopt;
        List<u32>      exit_fwd_patches;
    };

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // 单构造：enclosing 为 nullptr 即入口 <main> 上下文（enclosing_ == nullptr = entry），否则
        // 嵌套（指向外层）。kind 无默认值；实例方法族时槽 0 = 具名局部 this（见 FnKind 注）。
        explicit FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, FnKind kind);

        FunctionCtx(const FunctionCtx&)                = delete;
        FunctionCtx& operator=(const FunctionCtx&)     = delete;
        FunctionCtx(FunctionCtx&&) noexcept            = delete;
        FunctionCtx& operator=(FunctionCtx&&) noexcept = delete;

        // 压 Local{name, scope_depth_}，返回 slot（纯登记，不发射、不查重）。
        u16 add_local(StringView name);

        // 常量入池（带去重）：池内已有 === 同值常量即复用其索引，未命中追加并登记，返回池索引。
        // 纯登记不发射；池溢出不在本层判（由 CodeGen 预检后 fail）。
        u16 add_constant(Value value);

        // 当前作用域是否已定义同名局部（外层同名允许 shadow）。
        [[nodiscard]]
        bool is_defined_in_scope(StringView name) const;

        // 当前函数局部查表，返最近一个同名局部 slot（最内层）；未命中返 nullopt。
        [[nodiscard]]
        Opt<u16> find_local(StringView name) const;

        // upvalue 登记（M4 闭包）：同 (is_local,index) 已登记即复用其下标（同一局部被多处引用只占一个
        // upvalue），否则追加。追加将越出 u8 索引域（size > kMaxUpvalues）返 nullopt，由 CodeGen
        // fail(TooManyUpvalues)。纯登记，不发射。
        Opt<u8> add_upvalue(UpvalueDesc desc);

        void begin_scope();

        // 退出块作用域：--scope_depth_ 后弹出原 scope 的局部（depth > 新 scope_depth_，自最内层向外清），
        // 真正从编译期 locals_ 移除，后续语句不可再引用。break/continue 跳出循环不得走此--跳转后的语句
        // 仍在作用域内可引用这些局部，须保留登记。弹区清理指令由 CodeGen 先经 emit_pop_locals_to 发射
        // （需在登记移除前做），本方法只管登记收尾。
        void end_scope();

        // 成员（公开，CodeGen 直接访问 fn_/locals_/loop_stack_/upvalues_ 等）。enclosing_ 所有权：入口 fn
        // 上下文由 ModuleCtx 构造期 new、~ModuleCtx 沿链 delete；子上下文由 compile_function new（成功路径
        // delete、出错交 ~ModuleCtx 走链）。父编译期长于子，故 enclosing_ 裸指针在子生命期内稳定。
        FunctionCtx*      enclosing_;
        ObjFunction*      fn_;
        FnKind            kind_;
        List<Local>       locals_;
        u32               scope_depth_;
        Stack<LoopCtx>    loop_stack_;
        List<UpvalueDesc> upvalues_; // M4 闭包捕获描述表（按下标即 upvalue 索引），经 add_upvalue 逐条登记

        // 常量池去重索引（Value -> 池索引，键相等用 ===，见 Value.hpp 的 std 特化）。**编译期草稿**：每个
        // 函数一份、随本上下文销毁（池本体 fn_->unit().constants 才是产物）。不参与 GC（std 分配器）、不是
        // GC 根：键在池内都有同值副本，保活靠池。每函数一份是硬约束--共用一张表漏清一次就会从上一个函数的
        // 池里拿到索引，那是静默发射错常量。
        HashMap<Value, u16> constant_index_;
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
