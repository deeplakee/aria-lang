#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 字节码生成器：单遍遍历 AST（AstVisitor）同时完成名字解析 / 语义检查 / 字节码发射，ProgramNode ->
// 入口 ObjFunction；ExprNode visit 留一值、StmtNode visit 留零值。
// GC：compile() 入口 make_guard(module) 贯穿全程，建设中对象经 module 根链可达；入池前分配皆 trivial
// 不触 GC，new_string（intern）为 weak root，name 串由工厂 StringView 重载 intern 并自守。

#include "aria.hpp"
#include "bytecode/code.hpp"
#include "common.hpp"
#include "compile/AstVisitor.hpp"
#include "compile/FunctionCtx.hpp"
#include "compile/ModuleCtx.hpp"
#include "compile/ast.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjModule;
    class ObjFunction;
    class CodeUnit;

    class CodeGen final : public AstVisitor {
        // lvalue 发射模式：Load = 按读处理；Prepare = 普通赋值的第一步，只发射接收者；Store = 写入赋值
        // 目标（peek-store 留值）；Locate = 复合赋值/前置自增先定位一次（运行时 locator 的副本经 DUP/DUP2
        // 交给写入步复用，编译期常量 locator 与 Load 同形）。
        enum class LvalueMode : u8 { Load, Prepare, Store, Locate };

        // 解构绑定模式：Fill = 绑新名（值填槽）；Store = 写既有名（STORE_* 为 peek-store，补 POP 净耗
        // 一值）。由 bind_pattern 设置、整个模式子树经 accept 共用。
        enum class PatternBindMode : u8 { Fill, Store };

    public:
        // 编译 program 为 module 的入口 ObjFunction（arity 0，名 entry_name：主入口 kMainEntryName /
        // 导入模块 kModuleEntryName）。分配归属 gc；成功已 set_entry，失败返回首错 Error。
        static Result<ObjFunction*, Error> compile(GC& gc, const ProgramNode& program, ObjModule* module,
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

        LvalueMode      lvalue_mode_;
        PatternBindMode pattern_mode_;

        UPtr<ModuleCtx> mod_ctx_;

        // compile 的实例侧实现（单遍合一）。
        Result<ObjFunction*, Error> generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                      StringView entry_name);

        // 建入口函数 + set_entry + 构造 ModuleCtx；须在 module 已根化（guard）之后调用。
        ObjFunction* init_module(ObjModule* module, StringView entry_name);

        // 由 mod_ctx_ 游标派生，编译外（mod_ctx_ 为空）不可调用。
        [[nodiscard]]
        FunctionCtx* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // *_or_fail 家族：失败即 fail（[[noreturn]]），成功返解包值。

        // 池溢出 fail CodeUnitTooLarge，否则入池返回索引。
        [[nodiscard]]
        u16 add_constant_or_fail(Value value, SourceLoc loc) const;

        // intern name 成 ObjString 入池返回索引。
        [[nodiscard]]
        u16 add_name_or_fail(StringView name, SourceLoc loc) const;

        // 局部登记：同 scope 重名/溢出 fail（持 loc），返回槽位（= 值所在位置）。
        // 调用方保证值已压栈，登记即初始化。
        [[nodiscard]]
        u16 define_local_or_fail(StringView name, SourceLoc loc) const;

        // 全局绑定：同名已登记 fail RedefinedVariable；入池后 DEF_GLOBAL 弹栈值定义。
        void define_global_or_fail(StringView name, SourceLoc loc) const;

        void begin_scope() const;

        // 弹区清理先于登记移除（is_captured 判定需完整 locals_）。
        void end_scope(u32 line) const;

        // 弹区发射口：depth > target_depth 的局部尾段整区一条 POP_N（含被捕获局部），含被捕获局部
        // 才补 CLOSE_UPVALUE。只发射不改登记。
        void emit_pop_locals_to(u32 target_depth, u32 line) const;

        // 名字解析结果：index 随 kind 为局部槽 / upvalue 索引 / 名字池索引。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 裸名解析，序：局部 -> upvalue（递归登记捕获） -> 全局（入池，溢出即 fail）。
        [[nodiscard]]
        ResolvedVar resolve_name_or_fail(StringView name, SourceLoc loc);

        // this 专用解析（this 是关键字，永不落全局）：沿 ctx 链找 kThisName，链上无实例方法 fail。
        [[nodiscard]]
        ResolvedVar resolve_this_or_fail(SourceLoc loc);

        // 递归解析 ctx 体内引用 name 应捕获的 upvalue：外层局部命中 -> 置 is_captured + 登记
        // {is_local=true, slot}；穿透外层递归，命中登记 {is_local=false, 外层索引}；entry 之上返 nullopt。
        [[nodiscard]]
        Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);

        // add_upvalue 返 nullopt -> fail TooManyUpvalues；ctx 显式传因登记发生在链上各层。
        [[nodiscard]]
        u8 add_upvalue_or_fail(FunctionCtx* ctx, UpvalueDesc desc, SourceLoc loc) const;

        // 越界 fail CodeUnitTooLarge。
        void patch_jump_or_fail(u32 src_off, SourceLoc loc) const;

        // 条件跳转发射收口:条件恰为 `==` 二元时直发融合指令 JUMP_NE(与
        // [cond][JUMP_FALSE] 逐语义等价,省一次派发往返);其余形态原样 [cond][JUMP_FALSE]。
        // 融合判定必须在 AST 形状上做 -- 发射期「尾字节恰为 EQUAL」不等于「该 EQUAL 的值即
        // 本次条件」(|| 链 rhs 的 EQUAL 与外层跳转之间隔着值消费关系,按尾字节误融合会双弹
        // 操作数、还覆盖 || 已回填的链尾补丁地址)。返回占位偏移,契约同 emit_jump。
        u32 emit_cond_jump(ExprNode& cond, u32 line);

        void emit_jump_back_or_fail(u32 target_off, SourceLoc loc) const;

        // 循环收尾：先发 JUMP_BACK 回边（目标 back_target），再回填 exit -> L_end。for 的 continue
        // 回填须先于递增发射，不入本口（调用点处理）。
        void emit_loop_backedge_and_exits(const LoopCtx& loop_ctx, SourceLoc loc) const;

        // 栈顶值绑定收口（var/fun/def/import 共用）：全局 DEF_GLOBAL 弹值，局部值填槽。
        // 调用前值已在栈顶；var 初始化器先于此求值（init 同名引用沿 resolve 链落外层）。
        void bind_stack_value(StringView name, SourceLoc loc) const;

        // 左值种类校验：仅 Identifier/FieldAccess/IndexAccess 放行（发生在 rhs 求值之前）。
        void validate_lvalue_target(ExprNode& target) const;

        void emit_lvalue(ExprNode& node, LvalueMode mode);

        // 一次性 take：返回当前 lvalue_mode_ 并复位为 Load，防子节点泄漏。
        LvalueMode take_lvalue_mode();

        // this.x 且 this 为当前帧局部时走 THIS_FIELD 系；命中返回 true，未命中走经栈路径。
        [[nodiscard]]
        bool try_emit_this_field(const FieldAccessNode& node, LvalueMode mode, u32 line) const;

        // 两段式第一段：PREPARE_METHOD name；方法解析先于实参求值，[recv] -> [recv, target]。
        void emit_prepare_method(u16 name_idx, u32 line) const;

        // 栈顶 receiver 上的 0 参方法调用：PREPARE_METHOD + CALL_METHOD 0，[receiver] -> [retval]。
        void emit_method_call0(StringView name, SourceLoc loc) const;

        // 成员访问作 callee 则两段式发射并返回 true；其余形态不发射返回 false 走一般 CALL，
        // 方法解析仍先于实参求值。
        [[nodiscard]]
        bool try_emit_method_call(const CallNode& node);

        // 当前帧是否直接方法帧（槽 0 = 具名局部 this）。
        [[nodiscard]]
        bool is_in_method() const;

        // 按已解析变量发射读取。
        void emit_load_var(const ResolvedVar& var, u32 line) const;

        // 按已解析变量发射写入（peek-store 留栈顶值）。
        void emit_store_var(const ResolvedVar& var, u32 line) const;

        // 整数字面量 i48 值域唯一闸门：越界 fail NumberOutOfRange。
        void validate_int_literal(i64 value, SourceLoc loc) const;

        // 值落 LOAD_IMM 的 i8 域则发立即数加载并返回 true，域外返回 false 走常量池。
        [[nodiscard]]
        bool try_emit_load_imm(i64 value, u32 line) const;

        // 整数字面量编一条加载：LOAD_IMM 不命中则 LOAD_CONST；负字面量折叠经此发射负值。
        void emit_int_literal(i64 value, u32 line, SourceLoc loc) const;

        // 浮点字面量恒入池 LOAD_CONST；取负由调用方并进值。
        void emit_float_literal(f64 value, u32 line, SourceLoc loc) const;

        // 操作数为数值字面量则取负并进常量、发一条加载返回 true，否则 false 走 emit_expr + NEGATE。
        [[nodiscard]]
        bool try_emit_negated_literal(ExprNode& operand) const;

        // 模式绑定：置 pattern_mode_ 后交节点自身 visit。Fill 源值填新局部/顶层全局，Store 净耗栈顶
        // 一值（解构赋值调用点先 DUP 留表达式值）；模式对子树恒定，不 take。
        void bind_pattern(PatternNode& node, PatternBindMode mode);

        // 逐位置发射「备源值 -> 下标 -> LOAD_INDEX -> 绑定」（`_` 位跳过）；元素位/rest 位交自身 visit。
        // 备源值随模式与访问数不同（Fill 单次零指令 / Fill 复取隐藏局部 / Store DUP），经 push_source 传入。
        template<typename PushSource>
        void emit_list_pattern_accesses(ListPatternNode& node, u32 line, PushSource&& push_source);

        void emit_expr(ExprNode& node); // 留一值

        void emit_stmt(StmtNode& node); // 不留值

        // expr 空则 LOAD_NIL 兜底，否则 emit_expr（均留一值）。
        void emit_expr_or_nil(ExprNode* expr, u32 line);

        // 形参合法性：>kMaxArity / 重名即 fail；只读 params，loc 取声明节点（非 body）。
        void validate_params(const List<Param>& params, SourceLoc loc) const;

        // match 语义检查：通配臂后不得再有臂（死臂仅掩盖臂序 bug），首个死臂即 fail；只读 arms。
        template<typename Arm>
        void validate_match_arms(const List<Arm>& arms) const;

        // match 降糖总口：subject 求值一次驻栈跨臂，逐臂「DUP + 模式 + EQUAL + 未命中跳下臂」，
        // 兜底抛共享 MatchNoArm；臂体经 emit_arm_body 分派。定义在 .cpp。
        template<typename Node>
        void emit_match(Node& node);

        // 语句臂走 emit_stmt（净零值），表达式臂走 emit_expr（每臂恰一值）。
        void emit_arm_body(StmtNode& body);

        void emit_arm_body(ExprNode& body);

        // CLOSURE 后函数值的绑定/注册分派：具名 fun 绑定全局/局部，Lambda 留栈，方法三态就地注册
        // （静态 MAKE_STATIC / 实例方法族 MAKE_METHOD）。须在 CLOSURE 之后、子上下文建立之前调用
        // （嵌套具名函数递归自捕获）。
        void bind_function_value(FnKind kind, StringView name, SourceLoc loc) const;

        // MAKE_CLASS 后按声明位置分派类名的绑定：类体内（嵌套类）经 DUP2 复制 (外层类, 本类) 对后由 MAKE_STATIC
        // 挂进外层类的静态表，体内自引用只能走全路径 A.B；模块顶层经 DUP + DEF_GLOBAL 提前绑定（构造中途抛错
        // 后类名保持已绑定）；函数局部直接值填槽。顶层与类体两种绑定须与 visitDefDeclNode 尾部的 POP 配对。
        void bind_class_value(bool is_member, StringView name, SourceLoc loc) const;

        // 形参登记 + 缺省印章序言，单循环按声明序交错：判等命中未传才求值默认值换入，再登记本参数名；
        // 时序保证缺省表达式可引用前序参数，后序参数名解析结构性不可见。须在子上下文就位后调用。
        void compile_params(const List<Param>& params, SourceLoc loc);

        // 隐式返回尾：init 返回 this（实例化不变式 Foo() 得实例），ModuleEntry 返回模块对象，其余 nil。
        // kind 取 cur_fn_ctx()，须在目标上下文就位后调用。
        void emit_implicit_return(SourceLoc loc) const;

        // 编译一个函数：CLOSURE + 随 kind 的绑定/注册（收口 bind_function_value）+ 子上下文内编译体，
        // 完成后切回父上下文，函数值留在父栈。
        void compile_function(StringView name, const List<Param>& params, BlockNode& body, SourceLoc decl_loc,
                              FnKind kind);

        // 错误（抛 AriaCompileException，compile() 顶层 catch 翻译为 Result；throw 即 unwind，首错即止）。
        template<typename... Args>
        [[noreturn]]
        void fail(const ErrorCode code, const SourceLoc loc, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{Error::from_detail(code, loc, std::format(fmt, std::forward<Args>(args)...))};
        }
    };

} // namespace aria

#endif // ARIA_CODEGEN_HPP
