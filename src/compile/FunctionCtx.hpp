#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 单函数编译上下文：局部栈 / 作用域深度 / 循环上下文栈 / 捕获描述表 / 外层指针。值填槽模型：值先压栈，
// add_local 槽位 = 当前栈高 = 值所在位置，登记即初始化、不预占槽；loop_stack_ 随函数走。

#include "compile/FnKind.hpp"
#include "object/ObjFunction.hpp"
#include "type.hpp"

namespace aria {

    // 单函数 upvalue 上限：LOAD/STORE_UPVALUE 的 idx 为 u8，可寻址 0..kMaxUpvalues，故容量 = 上限 + 1。
    constexpr u32 kMaxUpvalues = kU8OperandMax;

    // this 局部名：实例方法帧槽 0 的具名局部；this 是关键字不撞用户名，嵌套引用即沿 ctx 链捕获其槽 0。
    constexpr StringView kThisName = "this";

    // 局部变量条目（slot 0 = 哑元 callee）。
    struct Local {
        String name;
        u32    depth       = 0;
        bool   is_captured = false;
    };

    // 循环上下文（break / continue 回填）。
    struct LoopCtx {
        u32            loop_scope_depth     = 0;            // 循环体 scope 深度（break/continue 弹局部至此）
        u32            back_target          = 0;            // 循环头（条件求值点）；回边与后向 continue 恒跳此
        Opt<List<u32>> continue_fwd_patches = std::nullopt; // 前向 continue -> L_incr；nullopt = 无前向通道（后向跳）
        List<u32>      exit_fwd_patches;                    // 前向退出占位（条件假跳 + break），收尾统一回填 -> L_end
    };

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // 单构造：enclosing = nullptr 为入口 <main> 上下文，否则嵌套；实例方法族槽 0 = 具名局部 this。
        explicit FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, FnKind kind);

        FunctionCtx(const FunctionCtx&)                = delete;
        FunctionCtx& operator=(const FunctionCtx&)     = delete;
        FunctionCtx(FunctionCtx&&) noexcept            = delete;
        FunctionCtx& operator=(FunctionCtx&&) noexcept = delete;

        // 压 Local{name, scope_depth_}，返回 slot（纯登记，不发射、不查重）。
        u16 add_local(StringView name);

        // 常量入池（带 === 去重，同值复用索引）；纯登记不发指令，池溢出不在此判。
        u16 add_constant(Value value);

        // 当前作用域是否已定义同名局部（外层同名允许 shadow）。
        [[nodiscard]]
        bool is_defined_in_scope(StringView name) const;

        // 当前函数局部查表，返最近一个同名局部 slot（最内层）；未命中返 nullopt。
        [[nodiscard]]
        Opt<u16> find_local(StringView name) const;

        // upvalue 登记：同 (is_local, index) 复用下标（同一局部多处引用只占一个 upvalue）；越 u8
        // 索引域（size > kMaxUpvalues）返 nullopt。纯登记，不发射。
        Opt<u8> add_upvalue(UpvalueDesc desc);

        void begin_scope();

        // 退出块作用域：弹出原 scope 的局部登记，后续语句不可再引用。break/continue 跳出循环不走此
        // （跳转后的语句仍在作用域内），弹区清理指令由调用方先经 emit_pop_locals_to 发射。
        void end_scope();

        // 父编译期长于子，enclosing_ 裸指针在子生命期内稳定。
        FunctionCtx*      enclosing_;
        ObjFunction*      fn_;
        FnKind            kind_;
        List<Local>       locals_;
        u32               scope_depth_;
        Stack<LoopCtx>    loop_stack_;
        List<UpvalueDesc> upvalues_; // 捕获描述表，下标即 upvalue 索引

        // 常量池去重索引（Value -> 池索引，键相等用 ===）。编译期草稿：每函数一份、随本上下文销毁；
        // 非 GC 根（std 分配器），键保活靠池内同值副本。每函数一份是硬约束：共用表漏清会取到上个
        // 函数池里的索引，静默发射错常量。
        HashMap<Value, u16> constant_index_;
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
