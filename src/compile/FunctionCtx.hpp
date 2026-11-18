#ifndef ARIA_FUNCTIONCTX_HPP
#define ARIA_FUNCTIONCTX_HPP

// 仅登记局部、作用域、捕获与常量，不发射指令；局部采用先压值再登记的填槽模型。

#include "compile/FnKind.hpp"
#include "object/ObjFnProto.hpp"
#include "type.hpp"

namespace aria {

    constexpr u32 kMaxUpvalues = kU8OperandMax;

    // 实例方法的槽 0 登记为 this，使嵌套函数能按局部捕获。
    constexpr StringView kThisName = "this";

    struct Local {
        StringView name; // 借自 AST 名字视图/kThisName/空哑元，存活覆盖编译期
        u32        depth       = 0;
        bool       is_captured = false;
    };

    struct LoopCtx {
        u32            loop_scope_depth     = 0;            // 循环体 scope 深度（break/continue 弹局部至此）
        u32            back_target          = 0;            // 循环头（条件求值点）；回边与后向 continue 恒跳此
        Opt<List<u32>> continue_fwd_patches = std::nullopt; // 前向 continue -> L_incr；nullopt = 无前向通道（后向跳）
        List<u32>      exit_fwd_patches;                    // 前向退出占位（条件假跳 + break），收尾统一回填 -> L_end
    };

    class FunctionCtx {
    public:
        FunctionCtx() = delete;

        // enclosing 为空表示入口函数；实例方法的槽 0 登记为 this。
        explicit FunctionCtx(ObjFnProto* proto, FunctionCtx* enclosing, FnKind kind);

        FunctionCtx(const FunctionCtx&)                = delete;
        FunctionCtx& operator=(const FunctionCtx&)     = delete;
        FunctionCtx(FunctionCtx&&) noexcept            = delete;
        FunctionCtx& operator=(FunctionCtx&&) noexcept = delete;

        u16 add_local(StringView name);

        // 按 === 去重，复用同值常量索引。
        u16 add_constant(Value value);

        [[nodiscard]]
        bool is_defined_in_scope(StringView name) const;

        [[nodiscard]]
        Opt<u16> find_local(StringView name) const;

        // 同一捕获复用索引；超出 u8 索引范围返回 nullopt。
        Opt<u8> add_upvalue(UpvalueDesc desc);

        void begin_scope();

        // 仅移除局部登记；调用方须先发射清理指令，break/continue 不改变编译期作用域。
        void end_scope();

        // 父上下文须比子上下文存活更久。
        FunctionCtx*      enclosing_;
        ObjFnProto*       proto_;
        FnKind            kind_;
        List<Local>       locals_;
        u32               scope_depth_;
        Stack<LoopCtx>    loop_stack_;
        List<UpvalueDesc> upvalues_; // 捕获描述表，下标即 upvalue 索引

        // 索引仅属于当前函数；键由常量池中的同值副本保活，本表不作 GC 根。
        HashMap<Value, u16> constant_index_;
    };

} // namespace aria

#endif // ARIA_FUNCTIONCTX_HPP
