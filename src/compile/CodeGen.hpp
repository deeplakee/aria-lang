#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 单遍完成名字解析、语义检查和字节码发射；表达式留一值，语句不留值。

#include "aria.hpp"
#include "bytecode/code.hpp"
#include "common.hpp"
#include "compile/Ast.hpp"
#include "compile/AstVisitor.hpp"
#include "compile/FunctionCtx.hpp"
#include "compile/ModuleCtx.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjModule;
    class ObjFnProto;
    class CodeUnit;

    class CodeGen final : public AstVisitor {
        // Prepare 仅求接收者；Locate 为读改写保留定位信息，避免重复求值。
        enum class LvalueMode : u8 { Load, Prepare, Store, Locate };

        // Fill 声明新绑定，Store 写既有目标；由 bind_pattern 设置并作用于整个模式子树。
        enum class PatternBindMode : u8 { Fill, Store };

        // 合成名不借用源缓冲，须单独持有稳定存储，供局部名视图借用。
        struct HiddenNameSource {
            u32                next_index = 0;
            List<UPtr<String>> names;

            StringView make_next() {
                names.push_back(std::make_unique<String>(std::format("<destructure_{}>", next_index)));
                ++next_index;
                return *names.back();
            }
        };

    public:
        // 成功后入口原型已挂到 module；失败返回首个 Error。
        static Result<ObjFnProto*, Error> compile(GC& gc, const ProgramNode& program, ObjModule* module,
                                                  StringView entry_name);

        ~CodeGen() override                    = default;
        CodeGen(const CodeGen&)                = delete;
        CodeGen& operator=(const CodeGen&)     = delete;
        CodeGen(CodeGen&&) noexcept            = delete;
        CodeGen& operator=(CodeGen&&) noexcept = delete;

        void visitProgramNode(ProgramNode& node) override;

        void visitBlockNode(BlockNode& node) override;
        void visitExprStmtNode(ExprStmtNode& node) override;
        void visitIfStmtNode(IfStmtNode& node) override;
        void visitWhileStmtNode(WhileStmtNode& node) override;
        void visitForStmtNode(ForStmtNode& node) override;
        void visitForInStmtNode(ForInStmtNode& node) override;
        void visitBreakStmtNode(BreakStmtNode& node) override;
        void visitContinueStmtNode(ContinueStmtNode& node) override;
        void visitReturnStmtNode(ReturnStmtNode& node) override;
        void visitImportStmtNode(ImportStmtNode& node) override;
        void visitTryStmtNode(TryStmtNode& node) override;
        void visitThrowStmtNode(ThrowStmtNode& node) override;
        void visitMatchStmtNode(MatchStmtNode& node) override;
        void visitFunDeclNode(FunDeclNode& node) override;
        void visitDefDeclNode(DefDeclNode& node) override;
        void visitVarDeclNode(VarDeclNode& node) override;
        void visitStaticVarMemberNode(StaticVarMemberNode& node) override;

        void visitIntegerLiteralNode(IntegerLiteralNode& node) override;
        void visitFloatLiteralNode(FloatLiteralNode& node) override;
        void visitStringLiteralNode(StringLiteralNode& node) override;
        void visitInterpolatedStringNode(InterpolatedStringNode& node) override;
        void visitBoolLiteralNode(BoolLiteralNode& node) override;
        void visitNilLiteralNode(NilLiteralNode& node) override;
        void visitIdentifierNode(IdentifierNode& node) override;
        void visitThisExprNode(ThisExprNode& node) override;
        void visitSuperExprNode(SuperExprNode& node) override;
        void visitBinaryExprNode(BinaryExprNode& node) override;
        void visitUnaryExprNode(UnaryExprNode& node) override;
        void visitAssignmentNode(AssignmentNode& node) override;
        void visitDestructureAssignmentNode(DestructureAssignmentNode& node) override;
        void visitCallNode(CallNode& node) override;
        void visitFieldAccessNode(FieldAccessNode& node) override;
        void visitIndexAccessNode(IndexAccessNode& node) override;
        void visitListExprNode(ListExprNode& node) override;
        void visitMapExprNode(MapExprNode& node) override;
        void visitRangeExprNode(RangeExprNode& node) override;
        void visitIfExprNode(IfExprNode& node) override;
        void visitLambdaExprNode(LambdaExprNode& node) override;
        void visitMatchExprNode(MatchExprNode& node) override;
        void visitSequenceExprNode(SequenceExprNode& node) override;

        void visitIdentifierPatternNode(IdentifierPatternNode& node) override;
        void visitWildcardPatternNode(WildcardPatternNode& node) override;
        void visitListPatternNode(ListPatternNode& node) override;

    private:
        explicit CodeGen(GC& gc) : gc_{gc}, lvalue_mode_{LvalueMode::Load}, pattern_mode_{PatternBindMode::Fill} {}

        GC& gc_;

        LvalueMode       lvalue_mode_;
        PatternBindMode  pattern_mode_;
        HiddenNameSource hidden_names_;
        UPtr<ModuleCtx>  mod_ctx_;

        Result<ObjFnProto*, Error> generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                     StringView entry_name);

        // 调用前 module 须已根化。
        ObjFnProto* init_module(ObjModule* module, StringView entry_name);

        [[nodiscard]]
        FunctionCtx* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // *_or_fail 失败时抛出编译错误，由 compile 统一捕获。

        [[nodiscard]]
        u16 add_constant_or_fail(Value value, SourceLoc loc) const;

        [[nodiscard]]
        u16 add_name_or_fail(StringView name, SourceLoc loc) const;

        [[nodiscard]]
        u16 add_string_literal_or_fail(const StringLiteralNode& node) const;

        // 先压值再登记局部，槽位即值所在位置；不预占槽。
        [[nodiscard]]
        u16 define_local_or_fail(StringView name, SourceLoc loc) const;

        void define_global_or_fail(StringView name, SourceLoc loc) const;

        void begin_scope() const;

        void end_scope(u32 line) const;

        // 只发射清理指令，不移除局部登记，供 break/continue 复用。
        void emit_pop_locals_to(u32 target_depth, u32 line) const;

        // index 依 kind 表示局部槽、upvalue 索引或名字常量索引。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 解析顺序：局部 -> upvalue -> 全局。
        [[nodiscard]]
        ResolvedVar resolve_name_or_fail(StringView name, SourceLoc loc);

        // this 可跨函数捕获，但不得落为全局名。
        [[nodiscard]]
        ResolvedVar resolve_this_or_fail(SourceLoc loc);

        [[nodiscard]]
        Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);

        [[nodiscard]]
        u8 add_upvalue_or_fail(FunctionCtx* ctx, UpvalueDesc desc, SourceLoc loc) const;

        void patch_jump_or_fail(u32 src_off, SourceLoc loc) const;

        // 仅按 AST 形状融合 == 条件。按末条指令推断会误吞短路表达式的内部比较。
        u32 emit_cond_jump(ExprNode& cond, u32 line);

        void emit_jump_back_or_fail(u32 target_off, SourceLoc loc) const;

        void emit_loop_backedge_and_exits(const LoopCtx& loop_ctx, SourceLoc loc) const;

        // 绑定栈顶值：全局定义会弹值，局部直接登记当前槽。
        void bind_stack_value(StringView name, SourceLoc loc) const;

        // 须在 rhs 求值前校验目标。
        void validate_lvalue_target(ExprNode& target) const;

        void emit_lvalue(ExprNode& node, LvalueMode mode);

        // 取出模式即复位，避免子节点继承父节点的左值模式。
        LvalueMode take_lvalue_mode();

        [[nodiscard]]
        bool try_emit_this_field(const FieldAccessNode& node, LvalueMode mode, u32 line) const;

        void emit_prepare_method(u16 name_idx, u32 line) const;

        void emit_method_call0(StringView name, SourceLoc loc) const;

        [[nodiscard]]
        bool try_emit_method_call(const CallNode& node);

        [[nodiscard]]
        bool is_in_method() const;

        void emit_load_var(const ResolvedVar& var, u32 line) const;

        void emit_store_var(const ResolvedVar& var, u32 line) const;

        void validate_int_literal(i64 value, SourceLoc loc) const;

        [[nodiscard]]
        bool try_emit_load_imm(i64 value, u32 line) const;

        void emit_int_literal(i64 value, u32 line, SourceLoc loc) const;

        void emit_float_literal(f64 value, u32 line, SourceLoc loc) const;

        [[nodiscard]]
        bool try_emit_negated_literal(ExprNode& operand) const;

        // Store 消耗栈顶源值；解构赋值须先 DUP 保留表达式结果。
        void bind_pattern(PatternNode& node, PatternBindMode mode);

        // push_source 负责复取源值；通配符位置不发射下标访问。
        template<typename PushSource>
        void emit_list_pattern_accesses(ListPatternNode& node, u32 line, PushSource&& push_source);

        void emit_expr(ExprNode& node); // 留一值

        void emit_stmt(StmtNode& node); // 不留值

        void emit_expr_or_nil(ExprNode* expr, u32 line);

        void validate_params(Span<Param> params, SourceLoc loc) const;

        template<typename Arm>
        void validate_match_arms(Span<Arm> arms) const;

        template<typename Node>
        void emit_match(Node& node);

        void emit_arm_body(StmtNode& body);

        void emit_arm_body(ExprNode& body);

        // 须在切入子上下文前绑定函数值，使递归函数能捕获自身。
        void bind_function_value(FnKind kind, StringView name, SourceLoc loc) const;

        // 类名须先于类体绑定以支持自引用；顶层和嵌套类的留栈值由类声明收尾弹出。
        void bind_class_value(bool is_member, StringView name, SourceLoc loc) const;

        // 每个默认值先求值再登记本参数，使其仅能引用前序参数。
        void compile_params(Span<Param> params, SourceLoc loc);

        void emit_implicit_return(SourceLoc loc) const;

        void compile_function(StringView name, Span<Param> params, BlockNode& body, SourceLoc decl_loc, FnKind kind);

        template<typename... Args>
        [[noreturn]]
        void fail(const ErrorCode code, const SourceLoc loc, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{Error::from_detail(code, loc, std::format(fmt, std::forward<Args>(args)...))};
        }
    };

} // namespace aria

#endif // ARIA_CODEGEN_HPP
