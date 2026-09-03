#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 字节码代码生成器：单遍遍历 AST（继承 AstVisitor），在一次访问中同时完成
//   - 名字解析（局部 / 模块全局；upvalue 留待 M4 闭包）
//   - 语义检查（首错即止，详见各 visit 内联检查）
//   - 字节码发射（经 cur_cu() 写当前函数的 CodeUnit；emit 编码逻辑下沉 CodeUnit）
// 把 ProgramNode 编译为模块入口 ObjFunction（arity 0，名 <main>，主入口模块体包装）。
//
// 设计要点：
//   - 单遍合一（clox 风格）：不另起 SemanticAnalyzer，resolve+check+emit 合一。
//   - 全骨架 + 可跑子集：42 个 visitXxxNode 全部 override；核心特性完整发射，
//     依赖未落地 VM 里程碑的特性（类 / 异常 / 闭包 / list / map / field / index 等）
//     占位 not_impl（编译期 NotImplemented Error），随 VM 推进逐个翻为真实发射。
//   - 首错即止：遇第一个语义错误记录并短路后续发射，compile() 返回 Result<ObjFunction*, Error>。
//
// 状态分离：每函数的可变状态（局部栈 / 作用域深度 / 循环上下文栈 / 外层链）收口于
//   FunctionCtx（见 compile/FunctionCtx.hpp）；每模块状态（模块句柄 + 当前函数上下文游标
//   current_fn_ctx_（兼拥有入口 fn 上下文：ctor new、dtor delete）+ 顶层全局名注册表）收口于 ModuleCtx
//   （见 compile/ModuleCtx.hpp），形成「模块 > 函数 > 作用域」三层。「当前函数」不再作 CodeGen 成员--
//   游标 current_fn_ctx_ 寄存于 ModuleCtx，CodeGen 经 cur_fn_ctx() 读取、compile_function 经
//   mod_ctx_->current_fn_ctx_ 摆动；「当前 CodeUnit」不再单独存，由 cur_cu() = &cur_fn_ctx()->fn_->unit()
//   派生，随游标自动切换。
//   CodeGen 持 UPtr<ModuleCtx> mod_ctx_（ModuleCtx 一次性、不可移动；compile 入口 make_unique、遍历后
//   reset() 即释放，~CodeGen 自动释放作安全网）。上下文所有权：current_fn_ctx_ 兼拥有入口 fn 上下文（ctor new、
//   ~ModuleCtx 沿 enclosing_ 链 delete）；compile_function 子上下文由 `new` 分配、靠 enclosing_ 链回父--
//   **成功**路径还原游标并手动 `delete` 子，**出错**路径 fail() 抛 AriaCompileException 直接 unwind（不还原游标、
//   不 delete 子），交 ~ModuleCtx 沿链释放。**错误通道**：与 Parser 同--编译期深层
//   fail() 抛 `AriaCompileException`（持 Error），自动 unwind 跨 visit 递归栈，compile() 顶层 catch 翻译为
//   `Result<ObjFunction*, Error>`（成功返入口函数，失败返 unexpected(e.error())）。无需 error_ 成员 / ok()
//   短路 / 各 visit 的 if(!ok()) return 守卫--throw 即 unwind，首个错误自然即止。unwind 时 compile_function 的
//   `delete child` 与还原游标被跳过，子留在 enclosing_ 链上，~ModuleCtx 析构沿链从游标走到 entry 逐个 delete
//   （成功时仅 entry，出错时整条活动链 + entry）。局部 / 作用域 / 循环 break-continue 的「登记」
//   由 FunctionCtx 负责（并返回弹出数等数据）；「发射」（emit_op / 跳转编码 / 回填 / 分块 / 槽位变体）下沉
//   CodeUnit，CodeGen 经 cur_cu()（派生自游标）调用。越界（超 64KB）由 CodeUnit 方法返 bool，CodeGen 翻译
//   为 Error。循环上下文随函数走，故 break/continue 不会跨函数绑定到外层循环。
//
// 栈契约：每个 visitXxxNode 自知契约--ExprNode 子类留一值，StmtNode 子类留零值。
// 父节点在 visit 体内显式调 emit_expr/emit_stmt 编排子节点；出错由 fail() 抛异常自动 unwind，无需逐调用短路。
//
// GC 安全：compile() 入口 gc_.make_guard(&module) 把 module 入临时根贯穿全程。经
//   module.entry_ -> 常量池 -> 嵌套 ObjFunction 常量池 -> ... 整链根化建设中 ObjFunction /
//   常量池 ObjString；每个子 fn 在 compile_function 起始即 add_constant 入父常量池(先于编译体)，
//   入池即经 module 根链可达。new_object -> add_constant 间走 trivial 分配(constants.push ->
//   reallocate)，按 GC 核心不变式不触发 GC，故 fn 跨该窗口无需守卫(见 GC.hpp)。真 GC 触发点
//   (new_object 顶部 maybe_collect)的守卫：工厂(new_function/new_native_fn/new_module)不再替
//   调用方守卫入参(「每方只守自己创建的」,工厂不创建入参),故调用方须自行 make_guard 根化传入的
//   module/name 入参；compile() 守 module 入临时根、compile_function 内部 intern name 成 ObjString*
//   并 make_guard 跨 new_function + 体编译（「每方只守自己创建的」:compile_function 创建 name_str
//   即自守,visit 层只传 StringView 无需守卫）。visitVarDeclNode（顶层）/visitImportStmtNode 的名字
//   经 add_name_or_fail 在 emit_expr 之后入池（new_string 结果立即 add_constant，trivial push 不
//   触发 GC，见 GC.hpp 核心不变式），无需守卫。

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
        // lvalue 模式（赋值/复合赋值 lowering，见 compound-assignment-lowering.md）：访问节点据此分支。
        // flag 由 emit_lvalue 设置、由目标节点在入口经 take_lvalue_mode() 一次性 take（取值并清空为 Load）--
        // 故子节点经 emit_expr 时 flag 已清空、不泄漏。emit_expr 入口 ASSERT lvalue_mode_ == Load，开发期
        // 捕获漏 take 的 bug（非预防性赋值）。构造与 compile() 入口亦置 Load（防上次 throw 残留跨复用）。
        //   Load   -- 默认 rvalue：visitIdentifierNode resolve + check_init[Local] + LOAD
        //   Store  -- 赋值目标：resolve + STORE + mark_init[Local]（peek-store 留值）
        //   Locate -- 预留：未来 Field/Index 单次求值 locator（receiver 经 DUP/DUP2 留栈，Load/Store
        //             复用栈上副本，非编译期 stash）。当前赋值只用 Load/Store，Locate 落入 Load 分支。
        enum class LvalueMode : u8 { Load, Store, Locate };

    public:
        CodeGen() = delete;

        // 以 VM 的 GC 构造（编译期分配的 ObjFunction / ObjString 归此 GC，与后续 run() 同源）。
        explicit CodeGen(GC& gc) : gc_{gc}, lvalue_mode_{LvalueMode::Load} {}

        // 编译 module 的顶层 ProgramNode 为入口 ObjFunction（arity 0、名 entry_name）。
        //   - entry_name：入口函数名（intern）。主入口模块传 <main>（默认）；运行期导入模块传 <module>
        //     （由 VM 加载层调用，区别于主入口，对齐 CPython 模块体 code object 同名）。
        // 整个编译期 module 入临时根（GC 启用，见上「GC 安全」）。成功返回入口函数（已 module.set_entry）；失败返回首错
        // Error。
        Result<ObjFunction*, Error> compile(const ProgramNode& program, ObjModule& module,
                                            StringView entry_name = "<main>");

        ~CodeGen() override                    = default;
        CodeGen(const CodeGen&)                = delete;
        CodeGen& operator=(const CodeGen&)     = delete;
        CodeGen(CodeGen&&) noexcept            = delete;
        CodeGen& operator=(CodeGen&&) noexcept = delete;

        // --- 根节点 ---
        void visitProgramNode(ProgramNode* node) override;

        // --- 语句节点 ---
        void visitBlockNode(BlockNode* node) override;
        void visitExprStmtNode(ExprStmtNode* node) override;
        void visitPrintStmtNode(PrintStmtNode* node) override;
        void visitIfStmtNode(IfStmtNode* node) override;
        void visitWhileStmtNode(WhileStmtNode* node) override;
        void visitForStmtNode(ForStmtNode* node) override;
        void visitForInStmtNode(ForInStmtNode* node) override;
        void visitBreakStmtNode(BreakStmtNode* node) override;
        void visitContinueStmtNode(ContinueStmtNode* node) override;
        void visitReturnStmtNode(ReturnStmtNode* node) override;
        void visitImportStmtNode(ImportStmtNode* node) override;
        void visitTryStmtNode(TryStmtNode* node) override;
        void visitThrowStmtNode(ThrowStmtNode* node) override;
        void visitMatchStmtNode(MatchStmtNode* node) override;
        void visitFunDeclNode(FunDeclNode* node) override;
        void visitDefDeclNode(DefDeclNode* node) override;
        void visitVarDeclNode(VarDeclNode* node) override;

        // --- 表达式节点 ---
        void visitIntegerLiteralNode(IntegerLiteralNode* node) override;
        void visitFloatLiteralNode(FloatLiteralNode* node) override;
        void visitStringLiteralNode(StringLiteralNode* node) override;
        void visitBoolLiteralNode(BoolLiteralNode* node) override;
        void visitNilLiteralNode(NilLiteralNode* node) override;
        void visitIdentifierNode(IdentifierNode* node) override;
        void visitThisExprNode(ThisExprNode* node) override;
        void visitSuperExprNode(SuperExprNode* node) override;
        void visitBinaryExprNode(BinaryExprNode* node) override;
        void visitUnaryExprNode(UnaryExprNode* node) override;
        void visitAssignmentNode(AssignmentNode* node) override;
        void visitDestructureAssignmentNode(DestructureAssignmentNode* node) override;
        void visitCallNode(CallNode* node) override;
        void visitFieldAccessNode(FieldAccessNode* node) override;
        void visitIndexAccessNode(IndexAccessNode* node) override;
        void visitListExprNode(ListExprNode* node) override;
        void visitMapExprNode(MapExprNode* node) override;
        void visitRangeExprNode(RangeExprNode* node) override;
        void visitIfExprNode(IfExprNode* node) override;
        void visitLambdaExprNode(LambdaExprNode* node) override;
        void visitMatchExprNode(MatchExprNode* node) override;

        // --- 解构模式节点 ---
        void visitIdentifierPatternNode(IdentifierPatternNode* node) override;
        void visitWildcardPatternNode(WildcardPatternNode* node) override;
        void visitListPatternNode(ListPatternNode* node) override;

    private:
        GC& gc_;

        // 当前 lvalue 模式（见类首 LvalueMode）：emit_lvalue 设置，目标节点入口经 take_lvalue_mode() 取值
        // 并清空为 Load；emit_expr 入口 ASSERT 之为 Load；构造与 compile() 入口均置 Load（防 throw 残留）。
        // 赋值/复合赋值/前置自增自减经 emit_lvalue 驱动目标节点，余经 emit_expr。
        LvalueMode lvalue_mode_;

        // 模块编译上下文（详见 compile/ModuleCtx.hpp）：UPtr 持有，compile 入口 make_unique、
        // 遍历后 reset() 即释放（无裸 delete）。~CodeGen 自动释放作安全网。编译期间非空，编译外为空。
        UPtr<ModuleCtx> mod_ctx_;

        // 模块初始化（compile 入口调用）：建入口函数（名 entry_name）+ set_entry + 构造 ModuleCtx（创建入口 fn 上下文、
        // 游标就位），返回入口函数。须在 module 已根化下调用（compile() 的 module_guard）。
        ObjFunction* init_module(ObjModule& module, StringView entry_name);

        // 当前函数上下文游标（= mod_ctx_->current_fn_ctx_）与当前 CodeUnit（由游标派生 =
        // &fn_->unit()，随 compile_function 摆动游标自动切换）。编译外（mod_ctx_ 为空）不可调用。
        [[nodiscard]]
        FunctionCtx* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // --- 常量池 / 局部 / 名字解析辅助（单层 _or_fail：操作 + 失败即 fail 并返回解包值）---
        // 原薄封装透传层（add_constant/add_name/declare_local/resolve_name，只做操作 + 失败信号、不持 loc）
        // 唯一消费者即对应 _or_fail，透传空转，故内联至此（见 .cpp）。add_name_or_fail 经 add_constant_or_fail
        // 复用溢出检查。失败即 fail（[[noreturn]]，之后值恒有效）；loc/message 由本层据节点 loc 显式构造。

        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge；否则入池返回索引。
        [[nodiscard]]
        u16 add_constant_or_fail(Value value, const SourceLoc& loc) const;

        // intern name 成 ObjString 入常量池，返回索引。溢出由 add_constant_or_fail fail。
        [[nodiscard]]
        u16 add_name_or_fail(StringView name, const SourceLoc& loc) const;

        // --- 局部管理（登记经 FunctionCtx，发射经 cur_cu()）---
        // 局部登记：检测重定义/溢出 -> fail（持 loc）；成功 add_local 仅登记并标「定义但未初始化」
        // （不发指令）。调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized。
        [[nodiscard]]
        u16 declare_local_or_fail(StringView name, const SourceLoc& loc) const;

        // cur_fn_ctx()->begin_scope()
        void begin_scope() const;

        // emit POP_N(= cur_fn_ctx()->end_scope_pop_count())
        void end_scope(u32 line) const;

        // emit POP_N(= cur_fn_ctx()->count_locals_deeper_than)（break/continue 弹比循环 scope
        // 更深的局部;仅计数不破坏 locals_ 登记--跳转后语句仍在作用域内可引用,故用 count 而非 pop）
        void pop_locals_to(u32 target_depth, u32 line) const;

        // --- 名字解析 ---
        // 名字解析结果：kind 描述命中类别，index 为相关槽/索引（Local: 局部槽；Global: 名字常量池索引；
        // Upvalue: 未用）。按值返回。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 裸名解析：当前函数局部命中 -> Local（index=局部槽）；外层函数局部 -> Upvalue（M4 未实现，调用方
        // emit_load_var/emit_store_var 走 not_impl）；否则视为模块全局 -> Global（index=名字常量池索引，VM
        // 运行期 LOAD_GLOBAL 查表，未定义报 UndefinedVariable）。Global 分支经 add_name_or_fail 入池，溢出
        // 即 fail（持 loc）。
        [[nodiscard]]
        ResolvedVar resolve_name_or_fail(StringView name, const SourceLoc& loc);

        // --- 跳转回填 / 全局登记失败翻译（void：仅翻译失败，无解包）---
        // 与上面 _or_fail 同一职责约定（操作 + 失败即 fail），但底层返 bool（patch_jump/emit_jump_back/
        // declare_global），无解包值，故为 void 封装。文案收口于此。

        // patch_jump 越界(跳转偏移超 u16 上限) -> fail CodeUnitTooLarge「跳转偏移超过 64KB」。
        void patch_jump_or_fail(usize src_off, const SourceLoc& loc) const;

        // emit_jump_back 越界(回边偏移超 u16 上限/反向) -> fail CodeUnitTooLarge「回边偏移超过 64KB」。
        // 比 patch_jump 多一个 line 参数--emit_jump_back 要发射 JUMP_BACK 指令(line 供其行号),而
        // patch_jump 只回填占位不发射,故无需 line。
        void emit_jump_back_or_fail(u32 target_off, u32 line, const SourceLoc& loc) const;

        // declare_global 已存在(重定义) -> fail RedefinedVariable「重复定义全局变量」。与
        // declare_local_or_fail 对称(局部/全局重定义检查各一),但 declare_global 返 bool、单一失败,故
        // 同上两者为 void 封装(无解包)。替代 visit 层 3 处 if+fail,消息文案收口于此。
        void declare_global_or_fail(StringView name, const SourceLoc& loc) const;

        // --- lvalue（复合赋值 lowering，见 compound-assignment-lowering.md）---
        // 验证赋值左值种类合法：Identifier/FieldAccess/IndexAccess 是合法左值种类（放行，由各自 visit 节点
        // 处理 load/store 或 not_impl）；其余节点种类 -> InvalidAssignmentTarget。由 emit_lvalue 在分派前
        // 调用：复合/前置自增自减的首次 emit_lvalue(Load) 先于 rhs，普通 = 的 emit_lvalue(Store) 后于 rhs
        // （非法左值在 rhs 编译后才抛，字节码随 throw 丢弃）。Field/Index 的 not_impl 由 visit 节点分派时抛。
        void validate_lvalue_target(ExprNode* target) const;

        // 以给定 lvalue 模式分派目标节点：先 validate_lvalue_target(n) 验证左值种类，再设置 lvalue_mode_ 后
        // n->accept(*this)（不在分派后恢复--清空职责交给目标节点的 take_lvalue_mode()）。复合赋值/前置自增
        // 自减用 Load+Store 两次分派（Identifier 重 resolve 廉价无副作用，locator-once 自然成立；两次分派
        // 会重复 validate，首次失败即抛，无正确性问题）。
        void emit_lvalue(ExprNode* n, LvalueMode m);

        // 目标节点（visitIdentifierNode 等）入口调用：返回当前 lvalue_mode_ 并清空为 Load（一次性 take）。
        // 节点据返回值分支 Load/Store；清空确保子节点经 emit_expr 时 flag 已为 Load、不泄漏。对 lvalue_mode_
        // 的直接读写收口于此与 emit_lvalue，访问节点不直接触碰该成员。
        LvalueMode take_lvalue_mode();

        // 在栈顶 receiver 上调用 0 参方法 name：LOAD_FIELD name; CALL 0。receiver 由调用方在调用前
        // 压栈（emit_expr / emit_load_local 等），调用后栈顶即方法返回值（[receiver] -> [retval]）。
        // 封装 for-in 的 iter()/has_next()/next() 三处同型 LOAD_FIELD+CALL 0 模式；name 入常量池经
        // add_name_or_fail（溢出即 fail）。将来 M5 类方法调用 lowering 可复用此原语。
        void emit_method_call0(StringView name, u32 line, const SourceLoc& loc) const;

        // 读点 init 检查：读未初始化局部 -> fail UninitializedVariable（definite-assignment）。
        // 仅做检查并报错，不发射。
        void check_local_initialized(u16 slot, const SourceLoc& loc) const;

        // 按已解析变量发射读取（Load / Locate）：Local 先读点 init 检查再 emit_load_local；Global LOAD_GLOBAL；
        // Upvalue -> not_impl（M4 闭包）。visitIdentifierNode 经 switch(mode) 分派至此。var.index 为局部槽或
        // 全局名字常量池索引；loc 供 check_local_initialized / not_impl（走其 SourceLoc 重载）复用。
        void emit_load_var(const ResolvedVar& var, u32 line, const SourceLoc& loc) const;

        // 按已解析变量发射写入（Store，peek-store 留栈顶值）：Local emit_store_local + mark_initialized
        // （赋值即初始化，不做 init 检查）；Global STORE_GLOBAL；Upvalue -> not_impl（M4 闭包）。
        void emit_store_var(const ResolvedVar& var, u32 line, const SourceLoc& loc) const;

        // --- 模式绑定（forIn 用）---
        // bind_pattern: 栈顶已有一值（for-in 的 next() 产物），按模式绑定为 per-iteration 局部。
        // IdentifierPattern -> declare_local 值填槽 + mark_initialized（不发指令）；WildcardPattern -> POP 丢弃；
        // ListPattern -> not_impl。行号取自 pat->loc_line()（仅 _/ListPattern 分支发射时用）。
        void bind_pattern(PatternNode* pat);

        // --- 遍历入口（薄包装：accept 双分派）---
        void emit_expr(ExprNode* n); // ASSERT lvalue_mode_ == Load 后 n->accept(*this)，留一值

        void emit_stmt(StmtNode* n); // n->accept(*this)，不留值

        // --- 函数编译（FunDecl / Lambda 共用）---
        // 形参合法性检查（compile_function 编译体前调用）：>kMaxArity -> TooManyParameters；默认参数 / varargs
        // -> not_impl；形参重名 -> DuplicateParam。只读 params、不触碰编译器状态，首错即 fail / not_impl 抛出。
        // loc 为声明节点位置（fun 关键字，compile_function 经 decl_loc 传入）而非 body->loc()（body 的 '{'），
        // 更贴近参数列表所在。只需位置无需整节点，故入参为 const SourceLoc& 而非 ASTNode*（not_impl 走其重载）。
        void validate_params(const List<Param>& params, const SourceLoc& loc) const;

        // name 为函数名 StringView（恒非空:具名 fun 为声明名、lambda 为 `<anonymous>`、入口为 `<main>`），
        // 内部 new_string intern 成 ObjString* 并 make_guard 跨 new_function + 体编译（每方只守自己创建的）。
        // name == `<anonymous>` -> lambda:函数值留栈不绑定名字;否则具名 fun 绑定到模块全局(顶层)或局部(嵌套)
        // （`<>` 标识符不可用,仅 visitLambdaExprNode 产生 `<anonymous>`,故 name 即 lambda 判据）。
        // body 为函数体 BlockNode;完成后切回父上下文。函数值已在父序列压栈（LOAD_CONST fn_idx）。
        // decl_loc 为声明节点位置（fun 关键字，visit 层经 node->loc() 传入），供 validate_params 报参数错;
        // 体发射行号仍取 body->loc_line()。只需位置无需整节点，故入参为 const SourceLoc& 而非 ASTNode*。
        void compile_function(StringView name, const List<Param>& params, BlockNode* body, const SourceLoc& decl_loc);

        // --- 错误（抛 AriaCompileException，compile() 顶层 catch 翻译为 Result）---
        template<typename... Args>
        [[noreturn]]
        void fail(ErrorCode code, const SourceLoc& loc, std::format_string<Args...> fmt, Args&&... args) const;

        [[noreturn]]
        void not_impl(ASTNode* node, StringView feature) const; // throw AriaCompileException(NotImplemented, loc, ...)

        // 同上，loc 直接传入（调用方仅有 SourceLoc 而无节点时用，如 validate_params）。
        [[noreturn]]
        void not_impl(const SourceLoc& loc, StringView feature) const;
    };

    // ------------------------------------------------------------
    // 错误模板：抛 AriaCompileException（首个错误自然即止--throw 即 unwind）
    // ------------------------------------------------------------
    template<typename... Args>
    [[noreturn]]
    void CodeGen::fail(ErrorCode code, const SourceLoc& loc, std::format_string<Args...> fmt, Args&&... args) const {
        throw AriaCompileException{Error::format(code, loc, fmt, std::forward<Args>(args)...)};
    }

} // namespace aria

#endif // ARIA_CODEGEN_HPP
