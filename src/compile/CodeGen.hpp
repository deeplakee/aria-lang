#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 字节码代码生成器：单遍遍历 AST（继承 AstVisitor），一次访问中同时完成名字解析（局部 /
//   upvalue / 模块全局，捕获即引用）、语义检查（首错即止）、字节码发射（经 cur_cu() 写当前
//   CodeUnit），把 ProgramNode 编译为模块入口 ObjFunction（arity 0）。
//
//   - 单遍合一（clox 风格）：不另起 SemanticAnalyzer，resolve+check+emit 合一。43 个
//     visitXxxNode 全部 override；仅解构族（DestructureAssignment/ListPattern）占位
//     not_impl（编译期 NotImplemented Error），随后续批翻为真实发射。
//   - 状态分离：每函数状态收口 FunctionCtx、每模块状态（含当前函数游标 current_fn_ctx_）
//     收口 ModuleCtx，形成「模块 > 函数 > 作用域」三层；「当前 CodeUnit」不单独存，由
//     cur_cu() = &cur_fn_ctx()->fn_->unit() 派生随游标切换。所有权：CodeGen 持
//     UPtr<ModuleCtx>（compile 入口 make_unique），出错路径交 ~ModuleCtx 沿 enclosing_ 链
//     释放，详见 ModuleCtx.hpp。
//   - **错误通道**：深层 fail() 抛 AriaCompileException（持 Error）自动 unwind 跨 visit
//     递归栈，compile() 顶层 catch 翻译为 Result -- 无需 error_ 成员 / ok() 短路守卫，
//     首个错误自然即止。
//   - 静态服务入口：compile(GC&, ...) 内部一次性构造（私有构造），无空态、无复用；
//     lvalue_mode_ 等状态构造置初值，无跨次残留。
//   - 栈契约：ExprNode 子类留一值、StmtNode 子类留零值；父节点经 emit_expr/emit_stmt 编排。
//
// GC 安全：compile() 入口 make_guard(module) 贯穿全程，建设中 ObjFunction/常量池经
//   module.entry_ 根链可达（子 fn 编译起始即 add_constant 入父池，先于编译体）；new_object
//   -> add_constant 间走 trivial 分配不触 GC（GC 核心不变式），fn 跨该窗口免守卫。真触发点
//   （new_object 顶 maybe_collect）前的入参根化：compile() 守 module；name 串由工厂
//   StringView 重载 intern 并自守（工厂守「自己创建的」），visit 层只传 StringView 无需守卫。

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
        // lvalue 模式（见 compound-assignment-lowering.md）：flag 由 emit_lvalue 设置、目标节点
        // 入口经 take_lvalue_mode() 一次性 take（取值并清空为 Load，子节点不泄漏；emit_expr 入口
        // ASSERT == Load 钉漏 take；构造置 Load，一次性对象无跨次残留）。Load = rvalue 读、
        // Prepare = 定位准备（只发接收者，普通 = 首腿；编译期常量 locator 目标恒 no-op）、
        // Store = 赋值目标（peek-store 留值）、Locate = 复合赋值/前置自增的定位腿。定位腿形态
        // 按 locator 性质分流：运行时 locator（一般对象/捕获 this 的 FieldAccess、IndexAccess）
        // 经 DUP/DUP2 副本跨腿复用；编译期常量 locator（Identifier、帧内 this.x）无副本可留，
        // 定位腿与 Load 同形（目标节点 switch 内折叠）。
        enum class LvalueMode : u8 { Load, Prepare, Store, Locate };

    public:
        // 静态服务入口：编译 module 的顶层 ProgramNode 为入口 ObjFunction（arity 0、名
        // entry_name：主入口 kMainEntryName（<main>）、运行期导入模块 kModuleEntryName（<module>，
        // 对齐 CPython 模块体 code object 同名），无默认值（调用方意图显式））。gc 为编译期分配的
        // ObjFunction / ObjString 归属（与后续 run() 同源）。内部一次性构造，无空态、无复用。
        // 成功返回入口函数（已 set_entry）；失败返回首错 Error。
        static Result<ObjFunction*, Error> compile(GC& gc, const ProgramNode& program, ObjModule* module,
                                                   StringView entry_name);

        ~CodeGen() override                    = default;
        CodeGen(const CodeGen&)                = delete;
        CodeGen& operator=(const CodeGen&)     = delete;
        CodeGen(CodeGen&&) noexcept            = delete;
        CodeGen& operator=(CodeGen&&) noexcept = delete;

        // --- 根节点 ---
        void visitProgramNode(ProgramNode& node) override;

        // --- 语句节点 ---
        void visitBlockNode(BlockNode& node) override;
        void visitExprStmtNode(ExprStmtNode& node) override;
        void visitPrintStmtNode(PrintStmtNode& node) override;
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

        // --- 表达式节点 ---
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

        // --- 解构模式节点 ---
        void visitIdentifierPatternNode(IdentifierPatternNode& node) override;
        void visitWildcardPatternNode(WildcardPatternNode& node) override;
        void visitListPatternNode(ListPatternNode& node) override;

    private:
        // 一次性实例：仅静态入口 compile 构造（编译期分配的 ObjFunction / ObjString 归 gc，
        // 与后续 run() 同源）。
        explicit CodeGen(GC& gc) : gc_{gc}, lvalue_mode_{LvalueMode::Load} {}

        GC& gc_;

        // 当前 lvalue 模式（语义与 take/set 纪律见类首 LvalueMode 注）。
        LvalueMode lvalue_mode_;

        // 模块编译上下文（所有权与生命期见类首「状态分离」段与 compile/ModuleCtx.hpp）。
        UPtr<ModuleCtx> mod_ctx_;

        // 静态入口 compile 的实例侧实现：把 ProgramNode 生成为字节码（单遍合一，名字解析
        // /语义检查随发射同遍完成）。
        Result<ObjFunction*, Error> generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                      StringView entry_name);

        // 模块初始化（generate_bytecode 入口调用）：建入口函数（名 entry_name）+ set_entry + 构造 ModuleCtx（创建入口
        // fn 上下文、 游标就位），返回入口函数。须在 module 已根化下调用（generate_bytecode 的 guard）。
        ObjFunction* init_module(ObjModule* module, StringView entry_name);

        // 当前函数上下文游标与当前 CodeUnit（均由 mod_ctx_ 游标派生，见类首「状态分离」段）。
        // 编译外（mod_ctx_ 为空）不可调用。
        [[nodiscard]]
        FunctionCtx* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // --- 常量池 / 局部 / 名字解析辅助（单层 _or_fail：操作 + 失败即 fail 并返回解包值）---
        // 失败即 fail（[[noreturn]]，之后值恒有效）；loc/message 由本层据节点 loc 显式构造，
        // 无中间薄封装层。add_name_or_fail 经 add_constant_or_fail 复用溢出检查。

        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge；否则入池返回索引。
        [[nodiscard]]
        u16 add_constant_or_fail(Value value, SourceLoc loc) const;

        // intern name 成 ObjString 入常量池，返回索引。溢出由 add_constant_or_fail fail。
        [[nodiscard]]
        u16 add_name_or_fail(StringView name, SourceLoc loc) const;

        // --- 局部管理（登记经 FunctionCtx，发射经 cur_cu()）---
        // 局部登记：检测重定义/溢出 -> fail（持 loc）；成功 add_local 仅登记（不发指令），返回
        // 登记槽位（= 值所在位置）。值填槽时序契约：调用方保证值已压栈，登记即初始化，无独立
        // init 状态。不标 [[nodiscard]]：值填槽调用方多数无需槽位（值已在槽），丢弃合法。
        [[nodiscard]]
        u16 define_local_or_fail(StringView name, SourceLoc loc) const;

        // cur_fn_ctx()->begin_scope()
        void begin_scope() const;

        // 退出作用域（块 / for / for-in / try 共用）：先 emit_pop_locals_to 发射弹区清理
        // （is_captured 判定需完整 locals_），再 FunctionCtx::end_scope() 收尾（移除登记）。
        void end_scope(u32 line) const;

        // 弹区清理统一发射口（退出作用域与 break/continue 共用）：自栈顶向外取 depth >
        // target_depth 的局部尾段（活局部按 depth 非递减序，slot 0 哑元恒在界外），整区一条
        // POP_N（被捕获局部一并计数）；弹区含被捕获局部才追加 CLOSE_UPVALUE（槽址 >= 新栈顶
        // 的开 upvalue 批量关闭，弹区槽已在新栈顶之上，不 push 不覆写即安全，对齐 Lua
        // OP_CLOSE)。只发射不改登记：退出作用域由 end_scope 收尾，break/continue 的登记本就
        // 须保留（跳转后语句仍在作用域内）。
        void emit_pop_locals_to(u32 target_depth, u32 line) const;

        // --- 名字解析 ---
        // 名字解析结果：kind 描述命中类别，index 为相关槽/索引（Local: 局部槽；Upvalue: upvalue
        // 索引；Global: 名字常量池索引）。按值返回。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 裸名解析：当前函数局部命中 -> Local（index=局部槽）；外层函数局部/外层 upvalue -> Upvalue
        // （index=本函数 upvalue 索引，resolve_upvalue 递归登记捕获描述）；否则视为模块全局 ->
        // Global（index=名字常量池索引，VM 运行期 LOAD_GLOBAL 查表，未定义报 UndefinedVariable）。
        // Global 分支经 add_name_or_fail 入池，溢出即 fail（持 loc）。
        [[nodiscard]]
        ResolvedVar resolve_name_or_fail(StringView name, SourceLoc loc);

        // this 专用解析（this 是关键字非标识符，永不落全局）：沿 fn ctx 链找名为 kThisName 的
        // 局部（最近实例方法的帧槽 0，arrow 语义）——当前帧命中 -> Local；外层命中 -> 经
        // resolve_upvalue 捕获（index=本函数 upvalue 索引，M4 机制零改动穿透）；链上无实例方法
        // -> fail ThisOutsideClass（静态方法/顶层/只嵌在普通函数里皆然）。
        [[nodiscard]]
        ResolvedVar resolve_this_or_fail(SourceLoc loc);

        // 递归解析「ctx 体内引用 name 应捕获的 upvalue」（clox resolveUpvalue）：先查
        // ctx->enclosing_ 的局部，命中 -> 置 is_captured + 登记 {is_local=true, slot}；未命中
        // -> 递归把 enclosing 当待捕获函数解析（穿透捕获），命中 -> 登记 {is_local=false, 外层
        // 索引}。返回 ctx 视角的 upvalue 索引；到 entry 之上无可捕获返 nullopt（调用方落全局）。
        // 登记经 add_upvalue_or_fail（容量越界 fail TooManyUpvalues）。
        [[nodiscard]]
        Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);

        // add_upvalue 失败翻译（单层 _or_fail 家族同款约定）：ctx 登记一条捕获描述，追加将越出
        // u8 索引域（add_upvalue 返 nullopt）-> fail TooManyUpvalues（持 loc），成功返回 upvalue 索引。
        // 入参 ctx 显式传入--resolve_upvalue 沿 enclosing_ 链递归，登记发生在链上各层（非恒 cur_fn_ctx）。
        [[nodiscard]]
        u8 add_upvalue_or_fail(FunctionCtx* ctx, UpvalueDesc desc, SourceLoc loc) const;

        // --- 跳转回填 / 全局登记失败翻译（void：仅翻译失败，无解包）---
        // 与上面 _or_fail 同一职责约定（操作 + 失败即 fail），但底层返 bool（patch_jump/emit_jump_back/
        // declare_global），无解包值，故为 void 封装。文案收口于此。

        // patch_jump 越界(跳转偏移超 u16 上限) -> fail CodeUnitTooLarge「跳转偏移超过 64KB」。
        void patch_jump_or_fail(u32 src_off, SourceLoc loc) const;

        // emit_jump_back 越界(回边偏移超 u16 上限/反向) -> fail CodeUnitTooLarge。要发射
        // JUMP_BACK 指令,行号现场取 loc(各调用点行号与 loc 同源,均取节点行)。
        void emit_jump_back_or_fail(u32 target_off, SourceLoc loc) const;

        // 循环收尾统一发射口（while/for/for-in 收尾共用）：先发 JUMP_BACK 回边（目标 =
        // loop_ctx.back_target，所有循环公有的循环头），再把 exit 回填列表逐个回填 -> L_end。
        // for 的 continue 前向回填须先于递增发射（时序见 visitForStmtNode），不入本口、由调用点
        // 先行处理。
        void emit_loop_backedge_and_exits(const LoopCtx& loop_ctx, SourceLoc loc) const;

        // declare_global 已存在(重定义) -> fail RedefinedVariable。
        void declare_global_or_fail(StringView name, SourceLoc loc) const;

        // 栈顶值绑定收口（var/fun/def/import 四处共用）：把栈顶的值按 name 绑为当前作用域的
        // 变量——全局 -> declare 判重 + add_name 入池 + DEF_GLOBAL 弹值定义；局部 -> 值填槽
        // declare（登记即初始化，值恰在槽位）。行号与判重/入池报错位置取 loc（声明行）现场求值。
        // 全局腿每次绑定各自入池（常量池不去重，与 resolve_name_or_fail 全局分支每次引用入池同
        // 常态）。var 的初始化器先于本调用求值（声明名在 init 求值后才登记，init 里的同名引用沿
        // resolve 链落外层），见 visitVarDeclNode。
        void bind_stack_value(StringView name, SourceLoc loc) const;

        // 验证赋值左值种类合法：Identifier/FieldAccess/IndexAccess 放行（由各自 visit 处理），
        // 其余 -> InvalidAssignmentTarget。两种赋值的首腿（普通 = 的 Prepare/复合的 Locate）均
        // 先于 rhs 抛错，字节码随 throw 丢弃。
        void validate_lvalue_target(ExprNode& target) const;

        // 以给定 lvalue 模式分派目标节点：先 validate_lvalue_target，再置 lvalue_mode_ 后
        // n.accept(*this)（清空职责交给目标节点的 take_lvalue_mode()）。
        void emit_lvalue(ExprNode& node, LvalueMode mode);

        // 目标节点入口调用：返回当前 lvalue_mode_ 并清空为 Load（一次性 take，防子节点泄漏）。
        // lvalue_mode_ 的读写收口于此与 emit_lvalue。
        LvalueMode take_lvalue_mode();

        // this.x 且 this 为当前帧局部（直接实例方法帧）的 THIS_FIELD 系发射（四模式，语义见类首
        // LvalueMode 注）：Prepare = no-op（this 不经栈，无接收者可备）；Load/Locate 同发
        // LOAD_THIS_FIELD（Locate 折叠：写腿不经栈取 this，槽 0 编译期常量）；Store =
        // STORE_THIS_FIELD（peek-store）。命中返回 true；一般对象/嵌套捕获 this 返回 false，
        // 交调用方走一般经栈路径。
        [[nodiscard]]
        bool try_emit_this_field(const FieldAccessNode& node, LvalueMode mode, u32 line) const;

        // 两段式第一段：PREPARE_METHOD name（名字索引 u16，与 LOAD_FIELD 等常量索引同宽）。
        // 解析在实参求值之前完成，待调值压在接收者之上（[recv] -> [recv, target]）。
        void emit_prepare_method(u16 name_idx, u32 line) const;

        // 在栈顶 receiver 上调用 0 参方法 name：PREPARE_METHOD name + CALL_METHOD 0（[receiver]
        // -> [retval]）。封装 for-in 的 iter()/has_next()/next() 三处同型模式（迭代器无 fields
        // 缓存，不绑定派发免去每迭代两次 bound 物化）。行号与 loc 同源，现场取（不双传）。
        void emit_method_call0(StringView name, SourceLoc loc) const;

        // recv.name(args) 两段式发射（visitCallNode 专用，与 emit_method_call0 同为
        // PREPARE_METHOD/CALL_METHOD 发射口，此口带实参）：命中「成员访问作 callee」形态则发
        // <recv> + PREPARE_METHOD name + <args> + CALL_METHOD argc 并返回 true；其余 callee 形态
        // （super 成员 SuperExprNode / 下标 / 调用结果等）不发射、返回 false，交调用方走
        // <callee> + args + CALL 一般路径。含 this.name(args)：接收者按通用路径压在调用区槽 0（与
        // THIS_FIELD 折叠相比指令数相同而省一次方法物化）。解析先于实参求值（语义见指令集 §5.6）。
        [[nodiscard]]
        bool try_emit_method_call(const CallNode& node);

        // 当前帧是否为直接方法帧（is_method(kind_)，槽 0 即具名局部 this）。visitSuperExprNode
        // （super.成员 语境检查）与 FieldAccess 的 THIS_FIELD 系分岔共用判据；「沿链找最近实例
        // 方法」的 this 捕获由 resolve_this_or_fail 承担，本谓词恒判当前帧。
        [[nodiscard]]
        bool is_in_method() const;

        // 按已解析变量发射读取（Load / Locate）：Local emit_load_local；Global LOAD_GLOBAL；
        // Upvalue LOAD_UPVALUE（u8 upvalue 索引；捕获时序语义同 Lua，与全局路径一致）。
        // visitIdentifierNode 经 switch(mode) 分派至此。var.index 为局部槽 / upvalue 索引 /
        // 全局名字常量池索引。
        void emit_load_var(const ResolvedVar& var, u32 line) const;

        // 按已解析变量发射写入（Store，peek-store 留栈顶值）：Local emit_store_local；Global
        // STORE_GLOBAL；Upvalue STORE_UPVALUE（u8 upvalue 索引，peek-store 写穿外层槽/已关值
        // --upvalue 索引非本帧局部槽）。
        void emit_store_var(const ResolvedVar& var, u32 line) const;

        // --- 数值字面量发射 ---
        // 整数字面量值域：越 i48 范围 -> fail NumberOutOfRange。发射路径唯一闸门--判的是字面量
        // 自身的值（负字面量折叠按取负后的值判，见 try_emit_negated_literal）。
        void validate_int_literal(i64 value, SourceLoc loc) const;

        // 立即数形态命中：value 落在 LOAD_IMM 的 i8 域内 -> 发一条带符号立即数加载并返回 true；
        // 域外不发射、返回 false（交调用方走常量池 LOAD_CONST）。
        [[nodiscard]]
        bool try_emit_load_imm(i64 value, u32 line) const;

        // 整数字面量编成一条加载指令：立即数形态（try_emit_load_imm）不命中则入池 LOAD_CONST。
        // 一元负号折叠经此发射负值。
        void emit_int_literal(i64 value, u32 line, SourceLoc loc) const;

        // 浮点字面量编成一条加载指令：恒入池 LOAD_CONST f64。取负由调用方并进值。
        void emit_float_literal(f64 value, u32 line, SourceLoc loc) const;

        // 负字面量形态命中（一元 - 的 Minus 臂专用）：操作数是数值字面量 -> 取负并进常量、发一条
        // 加载指令并返回 true；否则不发射、返回 false（交调用方走 emit_expr + NEGATE）。
        [[nodiscard]]
        bool try_emit_negated_literal(ExprNode& operand) const;

        // --- 模式绑定（forIn 用）---
        // bind_pattern: 栈顶已有一值（for-in 的 next() 产物），按模式绑定为 per-iteration 局部。
        // 值填槽模型：声明时值已在栈顶，slot = 当前栈高 = 值所在位置，值即该局部（无 STORE_LOCAL/POP）。
        // IdentifierPattern -> define_local_or_fail 值填槽（不发指令）；WildcardPattern -> POP
        // 丢弃； ListPattern -> not_impl。行号取自 pat.line()（仅 _/ListPattern 分支发射时用）。
        void bind_pattern(PatternNode& node);

        // --- 遍历入口（薄包装：accept 双分派）---
        void emit_expr(ExprNode& node); // ASSERT lvalue_mode_ == Load 后 n.accept(*this)，留一值

        void emit_stmt(StmtNode& node); // n.accept(*this)，不留值

        // 可选表达式发射：expr 非空 emit_expr（留一值），空则 LOAD_NIL 兜底（var/静态成员无初始化器
        // 填槽、return 缺省返回值共用）。line 由调用点定（声明行或逐绑定节点行），不从 expr 推。
        void emit_expr_or_nil(ExprNode* expr, u32 line);

        // --- 函数编译（FunDecl / Lambda / 类成员方法共用）---
        // 形参合法性检查（compile_function 编译体前调用）：>kMaxArity -> TooManyParameters；
        // 形参重名 -> DuplicateParam（varargs 恒末位由 Parser 结构性保证，rest 与默认参数
        // 共存的语义见 grammar.txt）。只读 params、不触碰编译器状态，首错即 fail 抛出。
        // loc 为声明节点位置（fun 关键字，compile_function 经 decl_loc 传入）而非 body.loc()（body 的 '{'），
        // 更贴近参数列表所在。
        void validate_params(const List<Param>& params, SourceLoc loc) const;

        // match 语义检查（emit_match 开头调用）：通配臂后不得再有臂——死臂任何输入下不可达，
        // 静默截断会吞臂序 bug，故编译期拒绝（_ 恒末臂、至多一条）。只读 arms、不触碰编译器状态，
        // 首个死臂即 fail（loc 取其 body）。模板吃两种臂类型（MatchArm/MatchExprArm 的
        // pattern/body 同形），定义在 .cpp（仅 CodeGen.cpp 实例化）。
        template<typename Arm>
        void validate_match_arms(const List<Arm>& arms) const;

        // match 降糖总口（两 visit 委派，模板吃 MatchStmtNode/MatchExprNode 同形字段 subject/arms）：
        // 先 validate_match_arms，随后 subject 求值一次、逐臂「DUP + 模式 + EQUAL + 未命中跳下臂」
        // 链 + 兜底抛共享 MatchNoArm（发射形态见 .cpp 定义处注释）。臂体经 emit_arm_body 按臂类型
        // 重载分派。定义在 .cpp（仅 CodeGen.cpp 实例化）。
        template<typename Node>
        void emit_match(Node& node);

        // emit_match 臂体分派：语句臂走 emit_stmt（净零值），表达式臂走 emit_expr（每臂恰一值）。
        void emit_arm_body(StmtNode& body);

        void emit_arm_body(ExprNode& body);

        // CLOSURE 后函数值的绑定/注册分派（穷尽 switch,-Wswitch 提示漏项；行号与报错位置现场
        // 取 decl_loc）：具名 fun（Function）绑定到全局（顶层）或局部（嵌套,值填槽,经
        // bind_stack_value）；Lambda 留栈作表达式值不绑定；方法三态留栈不绑定、就地注册——fun
        // 静态 MAKE_STATIC 不戳 defining class（静态槽读恒原值）,实例方法族 MAKE_METHOD 戳
        // （VM 侧方法性标记 + super 来源）。调用时序（CLOSURE 之后、子上下文建立之前）见
        // compile_function 注。
        void bind_function_value(FnKind kind, StringView name, SourceLoc loc) const;

        // 形参登记 + 缺省序言（印章方案）：单循环按声明序交错——带默认值的参数先发印章判等
        // 序言（未传槽判等命中才求值默认值 STORE_LOCAL 换入,语义见 cpp 注）、后 add_local 登记
        // 本参数名（slot 1..n,槽 0 = this/哑元）。交错时序保证缺省表达式可引用前序参数、自身/
        // 后序参数名字对解析结构性不可见。decl_loc 供序言行号与跳转回填报错（现场 .line() 求值）。
        // 须在子上下文就位后、体编译前调用。
        void compile_params(const List<Param>& params, SourceLoc loc);

        // 函数体尾隐式返回：init 方法返回 this（实例化不变式 Foo() 得实例——call_class 槽 0 原位换
        // 实例后以返回值为实例化结果，LOAD_LOCAL 0 即 this），其余返回 nil（显式 return 后为死
        // 代码，无害）。kind 读 cur_fn_ctx()->kind_，须在目标函数上下文就位后调用；行号由调用点定
        // （体尾取 body.line()，入口取 program.line()）。
        void emit_implicit_return(u32 line) const;

        // name 为函数名 StringView（具名 fun 声明名 / lambda kAnonymousName / 入口 `<main>` / 类成员
        // 方法名;`<>` 标识符不可用,合成名仅 VM 侧可达）。name 建串与守卫收口在工厂 StringView
        // 重载内（工厂守「自己创建的」,见类首 GC 安全注）。kind 为函数种类（见 FnKind.hpp）,决定
        // CLOSURE 后的绑定/注册分派（收口 bind_function_value）与帧形态:子上下文槽 0 形态（实例
        // 方法族 = 具名局部 this，见 is_method）;隐式返回尾（InitMethod 返回 this）。完成后切回父
        // 上下文,函数值已在父序列压栈（CLOSURE 按捕获描述表建 upvalue）。decl_loc 供 validate_params
        // 报参数错;声明区发射（CLOSURE/绑定/注册）的行号与报错位置统一取 decl_loc,隐式返回尾行号
        // 取 body.line()。无默认值,调用处显式写明。
        void compile_function(StringView name, const List<Param>& params, BlockNode& body, SourceLoc decl_loc,
                              FnKind kind);

        // --- 错误（抛 AriaCompileException，compile() 顶层 catch 翻译为 Result；throw 即 unwind，
        // 首个错误自然即止，详见类首「错误通道」注释）---
        template<typename... Args>
        [[noreturn]]
        void fail(const ErrorCode code, const SourceLoc loc, std::format_string<Args...> fmt, Args&&... args) const {
            throw AriaCompileException{Error::from_detail(code, loc, std::format(fmt, std::forward<Args>(args)...))};
        }

        [[noreturn]]
        // throw AriaCompileException(NotImplemented, loc, ...)
        void not_impl(const ASTNode& node, StringView feature) const;
    };

} // namespace aria

#endif // ARIA_CODEGEN_HPP
