#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 字节码代码生成器：单遍遍历 AST（继承 AstVisitor），在一次访问中同时完成
//   - 名字解析（局部 / 模块全局；upvalue 留待 M4 闭包）
//   - 语义检查（首错即止，详见各 visit 内联检查）
//   - 字节码发射（经 cur_cu() 写当前函数的 CodeUnit；emit 编码逻辑下沉 CodeUnit）
// 把 ProgramNode 编译为模块入口 ObjFunction（arity 0，匿名 <script>）。
//
// 设计要点：
//   - 单遍合一（clox 风格）：不另起 SemanticAnalyzer，resolve+check+emit 合一。
//   - 全骨架 + 可跑子集：42 个 visitXxxNode 全部 override；核心特性完整发射，
//     依赖未落地 VM 里程碑的特性（类 / 异常 / 闭包 / list / map / field / index 等）
//     占位 not_impl（编译期 NotImplemented Error），随 VM 推进逐个翻为真实发射。
//   - 首错即止：遇第一个语义错误记录并短路后续发射，compile() 返回 Result<ObjFunction*, Error>。
//
// 状态分离：每函数的可变状态（局部栈 / 作用域深度 / 循环上下文栈 / 外层链）收口于
//   FunctionContext（见 compile/FunctionContext.hpp）；每模块状态（模块句柄 + 当前函数上下文游标
//   current_fn_ctx_（兼拥有入口 fn 上下文：ctor new、dtor delete）+ 顶层全局名注册表）收口于 ModuleCtx
//   （见 compile/ModuleCtx.hpp），形成「模块 > 函数 > 作用域」三层。「当前函数」不再作 CodeGen 成员--
//   游标 current_fn_ctx_ 寄存于 ModuleCtx，CodeGen 经 cur_fn_ctx() 读取、compile_function 经
//   mod_ctx_->current_fn_ctx_ 摆动；「当前 CodeUnit」不再单独存，由 cur_cu() = &cur_fn_ctx()->fn_->unit()
//   派生，随游标自动切换。
//   CodeGen 持 UPtr<ModuleCtx> mod_ctx_（ModuleCtx 一次性、不可移动；compile 入口 make_unique、遍历后
//   reset() 即释放，~CodeGen 自动释放作安全网）。上下文所有权：current_fn_ctx_ 兼拥有入口 fn 上下文（ctor new、
//   ~ModuleCtx 沿 enclosing_ 链 delete）；compile_function 子上下文由 `new` 分配、靠 enclosing_ 链回父--
//   **成功**路径还原游标并手动 `delete` 子，**出错**路径 fail() 抛 AriaCompileException 直接 unwind（不还原游标、
//   不 delete 子），交 ~ModuleCtx 沿链释放。不再用 ctx_stack_ vector。**错误通道**：与 Parser 同--编译期深层
//   fail() 抛 `AriaCompileException`（持 Error），自动 unwind 跨 visit 递归栈，compile() 顶层 catch 翻译为
//   `Result<ObjFunction*, Error>`（成功返入口函数，失败返 unexpected(e.error())）。无需 error_ 成员 / ok()
//   短路 / 各 visit 的 if(!ok()) return 守卫--throw 即 unwind，首个错误自然即止。unwind 时 compile_function 的
//   `delete child` 与还原游标被跳过，子留在 enclosing_ 链上，~ModuleCtx 析构沿链从游标走到 entry 逐个 delete
//   （成功时仅 entry，出错时整条活动链 + entry）。局部 / 作用域 / 循环 break-continue 的「登记」
//   由 FunctionContext 负责（并返回弹出数等数据）；「发射」（emit_op / 跳转编码 / 回填 / 分块 / 槽位变体）下沉
//   CodeUnit，CodeGen 经 cur_cu()（派生自游标）调用。越界（超 64KB）由 CodeUnit 方法返 bool，CodeGen 翻译
//   为 Error。循环上下文随函数走，故 break/continue 不会跨函数绑定到外层循环。
//
// 栈契约：每个 visitXxxNode 自知契约--ExprNode 子类留一值，StmtNode 子类留零值。
// 父节点在 visit 体内显式调 emit_expr/emit_stmt 编排子节点；出错由 fail() 抛异常自动 unwind，无需逐调用短路。
//
// GC 安全：compile() 入口 gc_.make_lock() 贯穿全程，建设中 ObjFunction / 常量池 ObjString /
// 嵌套 ObjFunction 不会被 new_string / new_function / add_constant 触发的 maybe_collect 回收
// （与 AriaVM::run() 同一机制）。

#include "bytecode/code.hpp"
#include "common.hpp"
#include "compile/AstVisitor.hpp"
#include "compile/FunctionContext.hpp"
#include "compile/ModuleCtx.hpp"
#include "compile/ast.hpp"
#include "error/AriaException.hpp"
#include "error/Error.hpp"
#include "value/Value.hpp"

namespace aria {

    class GC;
    class ObjModule;
    class ObjFunction;
    class ObjString;
    class CodeUnit;

    class CodeGen final : public AstVisitor {
    public:
        CodeGen() = delete;

        // 以 VM 的 GC 构造（编译期分配的 ObjFunction / ObjString 归此 GC，与后续 run() 同源）。
        explicit CodeGen(GC& gc) : gc_{gc} {}

        // 编译 module 的顶层 ProgramNode 为入口 ObjFunction（arity 0，name=nullptr <script>）。
        // 整个编译期 GC 加锁。成功返回入口函数（已 module.set_entry）；失败返回首错 Error。
        Result<ObjFunction*, Error> compile(const ProgramNode& program, ObjModule& module);

        ~CodeGen() override                    = default; // mod_ctx_ 为 UPtr，自动释放（安全网）
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

        // 模块编译上下文（详见 compile/ModuleCtx.hpp）：UPtr 持有，compile 入口 make_unique、
        // 遍历后 reset() 即释放（无裸 delete）。~CodeGen 自动释放作安全网。编译期间非空，编译外为空。
        UPtr<ModuleCtx> mod_ctx_;

        // 模块初始化（compile 入口调用）：建入口函数 + set_entry + 构造 ModuleCtx（创建入口 fn 上下文、
        // 游标就位），返回入口函数。须在 gc_.make_lock() 下调用。
        ObjFunction* init_module(ObjModule& module);

        // 当前函数上下文游标（= mod_ctx_->current_fn_ctx_）与当前 CodeUnit（派生）。CodeGen 不再自持
        // 「当前函数」指针--游标在 ModuleCtx，cu 由游标派生（= &fn_->unit()），随 compile_function
        // 摆动游标自动切换，免两指针同步 save/restore。编译外（mod_ctx_ 为空）不可调用。
        [[nodiscard]]
        FunctionContext* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // --- 常量池辅助（emit 编码已下沉 CodeUnit，调用方经 cur_cu()->emit_* 直接发射）---
        // 薄封装：只做操作 + 失败信号，不构造/抛错误、不持 loc。溢出返回 nullopt，调用处检查后显式 fail。
        [[nodiscard]]
        Opt<u16> add_constant(Value v) const; // 溢出(>65535) -> nullopt；否则 cur_cu()->add_constant

        [[nodiscard]]
        Opt<u16> add_name(StringView s) const; // intern(new_string) + add_constant(from_obj)；溢出透传 nullopt

        // --- 局部管理（登记经 FunctionContext，发射经 cur_cu()）---
        // 薄封装：检测重定义/溢出 -> 返回 ErrorCode（不构造 Error、不持 loc）；成功 add_local 仅登记并标
        // 「定义但未初始化」（不发指令）。调用方在初始化器求值 / 无初始化器发 LOAD_NIL 后 mark_initialized。
        [[nodiscard]]
        Result<u16, ErrorCode> declare_local(StringView name) const;

        // = cur_fn_ctx()->find_local
        [[nodiscard]]
        Opt<u16> resolve_local(StringView name) const;

        // cur_fn_ctx()->begin_scope()
        void begin_scope() const;

        // emit POP_N(= cur_fn_ctx()->end_scope_pop_count())
        void end_scope(u32 line) const;

        // emit POP_N(= cur_fn_ctx()->pop_locals_deeper_than)（break/continue 弹比循环 scope
        // 更深的局部，仅副作用，返回计数无人用故 void）
        void pop_locals_to(u32 target_depth, u32 line) const;

        // --- 名字解析 ---
        // 名字解析结果：kind 描述命中类别，index 为相关槽/索引（Local: 局部槽；Global: 名字常量池索引；
        // Upvalue: 未用）。用结构体按值返回，取代旧 out_slot/out_name_idx 双引用参数的隐式返回。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 裸名解析：当前函数局部命中 -> Local（index=局部槽）；外层函数局部 -> Upvalue（M4 未实现，调用方 not_impl）；
        // 否则视为模块全局 -> Global（index=名字常量池索引，VM 运行期 LOAD_GLOBAL 查表，未定义报 UndefinedVariable）。
        // Global 分支 add_name 可能溢出 -> 透传 CodeUnitTooLarge，调用处检查后用节点 loc 显式 fail（本方法不持 loc）。
        Result<ResolvedVar, ErrorCode> resolve_name(StringView name);

        // --- lvalue（复合赋值 lowering，见 compound-assignment-lowering.md）---
        // 单 index 字段随 kind 解释（对齐 ResolvedVar「index 随 kind 重载」风格，取代旧 slot+name_idx 双字段）。
        //   Local:   局部槽
        //   Upvalue: upvalue 索引（M4 闭包未实现，compile_lvalue 走 not_impl）
        //   Global:  名字常量池索引
        //   Field:   field 名常量池索引（VM LOAD_FIELD/STORE_FIELD 未实现，走 not_impl）
        //   Index:   未用（obj/idx 运行时 locator，VM LOAD_INDEX/STORE_INDEX 未实现，走 not_impl）
        struct Lvalue {
            enum class Kind { Local, Upvalue, Global, Field, Index } kind;
            u16 index;
        };

        // 求 locator 描述符（identifier 类：编译期常量，不产生 load；Field/Index 运行时 locator 待 VM 落地）。
        // Upvalue/Field/Index -> not_impl；非 lvalue -> InvalidAssignmentTarget。
        Lvalue compile_lvalue(ExprNode* target);

        // Local: check_initialized + emit_load_local / Global: LOAD_GLOBAL
        void emit_load(const Lvalue& lv, u32 line, const SourceLoc& loc);

        // Local: emit_store_local + mark_initialized / Global: STORE_GLOBAL（peek-store 留值）
        void emit_store(const Lvalue& lv, u32 line) const;

        // 读点 init 检查：读未初始化局部 -> fail UninitializedVariable（definite-assignment）。
        // 仅做检查并报错，不发射。
        void check_local_initialized(u16 slot, const SourceLoc& loc);

        // --- 模式绑定（var 声明 / forIn 用）---
        // declare_pattern: 预留局部槽（IdentifierPattern -> declare_local；ListPattern -> not_impl）。
        void declare_pattern(PatternNode* pat);

        // bind_pattern: 栈顶已有一值，按模式绑定并弹栈。IdentifierPattern -> STORE_LOCAL k + POP；
        // WildcardPattern -> POP；ListPattern -> not_impl。行号取自 pat->loc()（绑定发生在模式源行）。
        void bind_pattern(PatternNode* pat);

        // --- 遍历入口（薄包装：accept 双分派）---
        void emit_expr(ExprNode* n); // n->accept(*this)，留一值

        void emit_stmt(StmtNode* n); // n->accept(*this)，不留值

        // --- 函数编译（FunDecl / Lambda 共用）---
        // name=nullptr -> 匿名（lambda / <script>）；body 为函数体 BlockNode。
        // 完成后切回父上下文。函数值已在父序列压栈（LOAD_CONST fn_idx）。
        void compile_function(ObjString* name, List<Param>& params, BlockNode* body);

        // --- 错误（抛 AriaCompileException，compile() 顶层 catch 翻译为 Result）---
        template<typename... Args>
        [[noreturn]]
        void fail(ErrorCode code, const SourceLoc& loc, std::format_string<Args...> fmt, Args&&... args);

        [[noreturn]]
        void not_impl(ASTNode* node, StringView feature); // throw AriaCompileException(NotImplemented, loc, ...)
    };

    // ------------------------------------------------------------
    // 错误模板：抛 AriaCompileException（首个错误自然即止--throw 即 unwind）
    // ------------------------------------------------------------
    template<typename... Args>
    [[noreturn]]
    void CodeGen::fail(ErrorCode code, const SourceLoc& loc, std::format_string<Args...> fmt, Args&&... args) {
        throw AriaCompileException{Error{code, loc, std::format(fmt, std::forward<Args>(args)...)}};
    }

} // namespace aria

#endif // ARIA_CODEGEN_HPP
