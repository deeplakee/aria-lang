#ifndef ARIA_CODEGEN_HPP
#define ARIA_CODEGEN_HPP

// 字节码代码生成器：单遍遍历 AST（继承 AstVisitor），一次访问中同时完成名字解析（局部 / upvalue / 模块全局，捕获即引用
// ）、语义检查（首错即止）、字节码发射（经 cur_cu() 写当前 CodeUnit），把 ProgramNode 编译为模块入口 ObjFunction（
// arity 0）。
//   - 状态分层：每函数状态收口 FunctionCtx、每模块状态（含当前函数游标）收口 ModuleCtx，成「模块 > 函数 > 作用域」三层
//     ；当前 CodeUnit 由 cur_cu() 派生不另存。所有权：CodeGen 持 UPtr<ModuleCtx>，出错路径交 ~ModuleCtx 沿 enclosing_
//     链释放（见 compile/ModuleCtx.hpp）。
//   - 错误通道：深层 fail() 抛 AriaCompileException（持 Error）自动 unwind 跨 visit 递归栈，compile() 顶层 catch 翻译
//     为 Result -- 无需 error_ 成员 / ok() 短路守卫，首错即止。栈契约：ExprNode 子类留一值、StmtNode 子类留零值；父节
//     点经 emit_expr/emit_stmt 编排。
//   - GC 安全：compile() 入口 make_guard(module) 贯穿全程，建设中 ObjFunction / 常量池经 module.entry_ 根链可达；
//     new_object -> add_constant 间走 trivial 分配不触 GC（GC 核心不变式）。new_string 结果是 weak root（intern 不保命
//     ），裸持跨真触发点须守 -- name 串由工厂 StringView 重载 intern 并自守，visit 层只传 StringView。

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
        // 入口经 take_lvalue_mode() 一次性 take（emit_expr 入口 ASSERT == Load 钉漏 take）。Load =
        // rvalue 读、Prepare = 只发接收者（普通 = 首腿，编译期常量 locator 目标恒 no-op）、Store =
        // 赋值目标（peek-store 留值）、Locate = 复合赋值/前置自增定位腿：运行时 locator 经 DUP/DUP2
        // 副本跨腿复用，编译期常量 locator 与 Load 同形（目标节点 switch 内折叠）。
        enum class LvalueMode : u8 { Load, Prepare, Store, Locate };

        // 解构绑定模式（发射形态见 .cpp visitListPatternNode 注）：Fill = 绑新名（var 声明 / for-in
        // 目标）--值填槽；Store = 写既有名（解构赋值目标）--STORE_* 为 peek-store，补 POP 弹掉取出的
        // 元素值。flag 由 bind_pattern 设置、整个模式子树共用（递归经 accept 双分派无参可传）。
        enum class PatternBindMode : u8 { Fill, Store };

    public:
        // 静态服务入口：编译 module 的顶层 ProgramNode 为入口 ObjFunction（arity 0、名 entry_name：
        // 主入口 kMainEntryName（<main>）、运行期导入模块 kModuleEntryName（<module>），无默认值）。
        // gc 为编译期分配的 ObjFunction / ObjString 归属（与后续 run() 同源）；成功返回入口函数
        // （已 set_entry），失败返回首错 Error。
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

        void visitIdentifierPatternNode(IdentifierPatternNode& node) override;
        void visitWildcardPatternNode(WildcardPatternNode& node) override;
        void visitListPatternNode(ListPatternNode& node) override;

    private:
        // 一次性实例：仅静态入口 compile 构造（分配的 ObjFunction / ObjString 归 gc）。
        explicit CodeGen(GC& gc) : gc_{gc}, lvalue_mode_{LvalueMode::Load}, pattern_mode_{PatternBindMode::Fill} {}

        GC& gc_;

        // 当前 lvalue 模式（语义与 take/set 纪律见类首 LvalueMode 注）。
        LvalueMode lvalue_mode_;

        // 当前解构绑定模式（语义见类首 PatternBindMode 注；模式只经 bind_pattern 设置）。
        PatternBindMode pattern_mode_;

        // 模块编译上下文（所有权与生命期见类首「状态分离」段与 compile/ModuleCtx.hpp）。
        UPtr<ModuleCtx> mod_ctx_;

        // 静态入口 compile 的实例侧实现：把 ProgramNode 生成为字节码（单遍合一）。
        Result<ObjFunction*, Error> generate_bytecode(const ProgramNode& program, ObjModule* module,
                                                      StringView entry_name);

        // generate_bytecode 入口调用：建入口函数（名 entry_name）+ set_entry + 构造 ModuleCtx（创建
        // 入口 fn 上下文、游标就位）；须在 module 已根化下调用（generate_bytecode 的 guard）。
        ObjFunction* init_module(ObjModule* module, StringView entry_name);

        // 当前函数上下文游标与当前 CodeUnit（均由 mod_ctx_ 游标派生）；编译外（mod_ctx_ 为空）不可调用。
        [[nodiscard]]
        FunctionCtx* cur_fn_ctx() const noexcept;

        [[nodiscard]]
        CodeUnit* cur_cu() const noexcept;

        // 常量池 / 局部 / 名字解析辅助（单层 _or_fail：操作 + 失败即 fail 并返回解包值；fail 为
        // [[noreturn]]，之后值恒有效；loc/message 据节点 loc 显式构造，无中间薄封装层）

        // 常量池溢出(>kMaxConstants) -> fail CodeUnitTooLarge；否则入池返回索引。
        [[nodiscard]]
        u16 add_constant_or_fail(Value value, SourceLoc loc) const;

        // intern name 成 ObjString 入常量池，返回索引。溢出由 add_constant_or_fail fail。
        [[nodiscard]]
        u16 add_name_or_fail(StringView name, SourceLoc loc) const;

        // 局部管理（登记经 FunctionCtx，发射经 cur_cu()）
        // 局部登记：同 scope 重名/溢出 -> fail（持 loc）；成功 add_local 仅登记不发指令，返回登记槽位
        // （= 值所在位置）。值填槽：调用方保证值已压栈，登记即初始化；不要槽位处以 std::ignore 丢弃。
        [[nodiscard]]
        u16 define_local_or_fail(StringView name, SourceLoc loc) const;

        // 全局绑定：同名全局已登记 -> fail RedefinedVariable；add_name 入池后发 DEF_GLOBAL 弹栈值
        // 绑定全局（值已在栈顶，弹值即定义）。行号与报错位置取 loc。
        void define_global_or_fail(StringView name, SourceLoc loc) const;

        void begin_scope() const;

        // 退出作用域（块 / for / for-in / try 共用）：先 emit_pop_locals_to 发射弹区清理
        // （is_captured 判定需完整 locals_），再 FunctionCtx::end_scope() 收尾（移除登记）。
        void end_scope(u32 line) const;

        // 弹区清理统一发射口（退出作用域与 break/continue 共用）：取 depth > target_depth 的局部尾段
        // 整区一条 POP_N（被捕获局部一并计数），弹区含被捕获局部才追加 CLOSE_UPVALUE（批量关闭对齐
        // Lua OP_CLOSE）。只发射不改登记：退出作用域由 end_scope 收尾，break/continue 的登记须保留。
        void emit_pop_locals_to(u32 target_depth, u32 line) const;

        // 名字解析结果：kind 描述命中类别，index 为相关槽/索引（Local: 局部槽；Upvalue: upvalue
        // 索引；Global: 名字常量池索引）。
        struct ResolvedVar {
            enum class Kind { Local, Upvalue, Global } kind;
            u16 index;
        };

        // 裸名解析（序：局部 -> upvalue -> 全局）：当前函数局部 -> Local；外层 -> Upvalue（经
        // resolve_upvalue 递归登记捕获描述）；否则 Global（名字经 add_name_or_fail 入池，溢出即 fail）。
        [[nodiscard]]
        ResolvedVar resolve_name_or_fail(StringView name, SourceLoc loc);

        // this 专用解析（this 是关键字非标识符，永不落全局）：沿 fn ctx 链找 kThisName 的局部，当前帧
        // -> Local、外层 -> 经 resolve_upvalue 捕获；链上无实例方法 -> fail ThisOutsideClass。
        [[nodiscard]]
        ResolvedVar resolve_this_or_fail(SourceLoc loc);

        // 递归解析「ctx 体内引用 name 应捕获的 upvalue」（clox resolveUpvalue）：直接外层局部命中 ->
        // 置 is_captured + 登记 {is_local=true, slot}；未命中 -> 把 enclosing 当待捕获函数递归（穿透
        // 捕获），命中 -> 登记 {is_local=false, 外层索引}。到 entry 之上返 nullopt（调用方落全局）。
        [[nodiscard]]
        Opt<u8> resolve_upvalue(FunctionCtx* ctx, StringView name, SourceLoc loc);

        // add_upvalue 失败翻译（单层 _or_fail 家族同款）：追加将越出 u8 索引域（add_upvalue 返
        // nullopt）-> fail TooManyUpvalues（持 loc），成功返回索引。ctx 显式传入（登记发生在链上各层）。
        [[nodiscard]]
        u8 add_upvalue_or_fail(FunctionCtx* ctx, UpvalueDesc desc, SourceLoc loc) const;

        // 跳转回填 / 全局登记失败翻译（void：仅翻译失败，无解包）：与上面 _or_fail 同一职责约定，
        // 但底层返 bool（patch_jump/emit_jump_back/declare_global），故为 void 封装；文案收口于此。

        // patch_jump 越界(跳转偏移超 u16 上限) -> fail CodeUnitTooLarge「function too large: jump offset exceeds
        // ...」。
        void patch_jump_or_fail(u32 src_off, SourceLoc loc) const;

        // emit_jump_back 越界(回边偏移超 u16 上限/反向) -> fail CodeUnitTooLarge；行号现场取 loc。
        void emit_jump_back_or_fail(u32 target_off, SourceLoc loc) const;

        // 循环收尾统一发射口（while/for/for-in 共用）：先发 JUMP_BACK 回边（目标 loop_ctx.back_target），
        // 再把 exit 回填列表逐个回填 -> L_end。for 的 continue 前向回填须先于递增发射、不入本口，由
        // 调用点先处理（时序见 visitForStmtNode）。
        void emit_loop_backedge_and_exits(const LoopCtx& loop_ctx, SourceLoc loc) const;

        // 栈顶值绑定收口（var/fun/def/import 四处共用）：全局 -> define_global_or_fail（判重 +
        // DEF_GLOBAL 弹值）；局部 -> define_local_or_fail（值填槽，登记即初始化，值恰在槽位）。
        // var 的初始化器先于本调用求值（init 里的同名引用沿 resolve 链落外层），见 visitVarDeclNode。
        void bind_stack_value(StringView name, SourceLoc loc) const;

        // 验证赋值左值种类合法：Identifier/FieldAccess/IndexAccess 放行，其余 -> InvalidAssignmentTarget；
        // 两种赋值的首腿（普通 = 的 Prepare / 复合的 Locate）均先于 rhs 抛错，字节码随 throw 丢弃。
        void validate_lvalue_target(ExprNode& target) const;

        // 以给定 lvalue 模式分派目标节点：先 validate_lvalue_target，再置 lvalue_mode_ 后 n.accept。
        void emit_lvalue(ExprNode& node, LvalueMode mode);

        // 目标节点入口调用：返回当前 lvalue_mode_ 并清空为 Load（一次性 take，防子节点泄漏）；
        // lvalue_mode_ 的读写收口于此与 emit_lvalue。
        LvalueMode take_lvalue_mode();

        // this.x 且 this 为当前帧局部（直接实例方法帧）的 THIS_FIELD 系发射（四模式）：Prepare = no-op
        // （this 不经栈）；Load/Locate 同发 LOAD_THIS_FIELD（Locate 折叠）；Store = STORE_THIS_FIELD
        // （peek-store）。命中返回 true，一般对象/嵌套捕获 this 返回 false 交调用方走经栈路径。
        [[nodiscard]]
        bool try_emit_this_field(const FieldAccessNode& node, LvalueMode mode, u32 line) const;

        // 两段式第一段：PREPARE_METHOD name（名字索引 u16）。解析在实参求值之前完成，待调值压在接收者
        // 之上（[recv] -> [recv, target]）。
        void emit_prepare_method(u16 name_idx, u32 line) const;

        // 在栈顶 receiver 上调用 0 参方法 name：PREPARE_METHOD name + CALL_METHOD 0（[receiver]
        // -> [retval]）。封装 for-in 的 iter()/has_next()/next() 三处同型模式；行号与 loc 同源现场取。
        void emit_method_call0(StringView name, SourceLoc loc) const;

        // recv.name(args) 两段式发射（visitCallNode 专用）：命中「成员访问作 callee」形态则发
        // <recv> + PREPARE_METHOD name + <args> + CALL_METHOD argc 并返回 true；其余 callee 形态不发射、
        // 返回 false 交调用方走 <callee> + args + CALL。解析先于实参求值（语义见指令集 §5.6）。
        [[nodiscard]]
        bool try_emit_method_call(const CallNode& node);

        // 当前帧是否为直接方法帧（is_method(kind_)，槽 0 即具名局部 this）；visitSuperExprNode（super
        // 语境检查）与 FieldAccess 的 THIS_FIELD 系分岔共用判据。
        [[nodiscard]]
        bool is_in_method() const;

        // 按已解析变量发射读取（Load / Locate）：Local emit_load_local；Global LOAD_GLOBAL；Upvalue
        // LOAD_UPVALUE（u8 索引，捕获时序语义同 Lua）。var.index 为局部槽 / upvalue 索引 / 名字池索引。
        void emit_load_var(const ResolvedVar& var, u32 line) const;

        // 按已解析变量发射写入（Store，peek-store 留栈顶值）：Local emit_store_local；Global
        // STORE_GLOBAL；Upvalue STORE_UPVALUE（写穿外层槽/已关值）。
        void emit_store_var(const ResolvedVar& var, u32 line) const;

        // 整数字面量值域：越 i48 范围 -> fail NumberOutOfRange。发射路径唯一闸门--判的是字面量自身的值。
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

        // 模式绑定（var 声明 / for-in 目标 / 解构赋值共用）
        // bind_pattern: 栈顶已有一值（var 初始化器 / for-in 的 next() 产物 / 解构赋值右值），置模式后
        // 交节点自身 visit 发射。Fill 把源值填成新局部（或顶层全局 DEF_GLOBAL 弹值），Store 净消耗栈顶
        // 一值（故解构赋值调用点先 DUP 一份留作表达式值）。模式对子树恒定，不 take/不清空。
        void bind_pattern(PatternNode& node, PatternBindMode mode);

        // listPattern 逐位置发射「备源值 -> 压下标 -> LOAD_INDEX -> 绑定」（`_` 位置跳过--不访问该位置）：
        // 元素位与 rest 位皆交自身 visit 递归绑定。备源值方式随模式与访问数不同（Fill 单次访问零指令 /
        // Fill 多次 LOAD_LOCAL 复取隐藏局部 / Store DUP），由 push_source 给出，故循环体只写一次。
        template<typename PushSource>
        void emit_list_pattern_accesses(ListPatternNode& node, u32 line, PushSource&& push_source);

        // 遍历入口（薄包装：accept 双分派）
        void emit_expr(ExprNode& node); // ASSERT lvalue_mode_ == Load 后 n.accept(*this)，留一值

        void emit_stmt(StmtNode& node); // n.accept(*this)，不留值

        // 可选表达式发射：expr 非空 emit_expr（留一值），空则 LOAD_NIL 兜底（var/静态成员无初始化器
        // 填槽、return 缺省返回值共用）。line 由调用点定。
        void emit_expr_or_nil(ExprNode* expr, u32 line);

        // 函数编译（FunDecl / Lambda / 类成员方法共用）
        // 形参合法性检查（compile_function 编译体前调用）：>kMaxArity -> TooManyParameters；形参重名 ->
        // DuplicateParam。只读 params、不触碰编译器状态，首错即 fail 抛出。loc 取声明节点位置（fun 关键
        // 字）而非 body.loc()，更贴近参数列表所在。
        void validate_params(const List<Param>& params, SourceLoc loc) const;

        // match 语义检查（emit_match 开头调用）：通配臂后不得再有臂（死臂任何输入下不可达，静默截断会
        // 吞臂序 bug，故编译期拒绝）；只读 arms，首个死臂即 fail（loc 取其 body）。模板吃两种臂类型。
        template<typename Arm>
        void validate_match_arms(const List<Arm>& arms) const;

        // match 降糖总口（两 visit 委派，模板吃 MatchStmtNode/MatchExprNode 同形字段）：先
        // validate_match_arms，随后 subject 求值一次、逐臂「DUP + 模式 + EQUAL + 未命中跳下臂」链 +
        // 兜底抛共享 MatchNoArm；臂体经 emit_arm_body 重载分派。定义在 .cpp。
        template<typename Node>
        void emit_match(Node& node);

        // emit_match 臂体分派：语句臂走 emit_stmt（净零值），表达式臂走 emit_expr（每臂恰一值）。
        void emit_arm_body(StmtNode& body);

        void emit_arm_body(ExprNode& body);

        // CLOSURE 后函数值的绑定/注册分派（穷尽 switch,-Wswitch 提示漏项；行号与报错位置取 decl_loc）：
        // 具名 fun 绑定到全局（顶层）或局部（嵌套,值填槽）；Lambda 留栈作表达式值不绑定；方法三态留栈
        // 不绑定、就地注册即消费（静态 MAKE_STATIC / 实例方法族 MAKE_METHOD）。须在 CLOSURE 之后、子
        // 上下文建立之前调用（嵌套具名函数递归自捕获）。
        void bind_function_value(FnKind kind, StringView name, SourceLoc loc) const;

        // MAKE_CLASS 后类名绑定分派（镜像 bind_function_value）：全局腿 DUP + DEF_GLOBAL 提前入全局
        // （类体前绑定，对齐 clox classDeclaration 的 OP_CLASS+OP_DEFINE_GLOBAL 先于类体与函数先例：
        // 构建窗口内裸名经全局解析到构建中类对象，初始化器 throw 后类名保持绑定 = 残留语义）；局部腿
        // （块内/函数内）值填槽预登记。全局腿配对义务：visitDefDeclNode ⑤ 须补 POP 弹掉驻留类值。
        void bind_class_value(StringView name, SourceLoc loc) const;

        // 形参登记 + 缺省序言（印章方案）：单循环按声明序交错--带默认值的参数先发印章判等序言（未传槽
        // 命中才求值默认值 STORE_LOCAL 换入）、后 add_local 登记本参数名（slot 1..n）；交错时序保证缺省
        // 表达式可引用前序参数、自身/后序参数名对解析结构性不可见。须在子上下文就位后、体编译前调用。
        void compile_params(const List<Param>& params, SourceLoc loc);

        // 函数体尾隐式返回：init 方法返回 this（LOAD_LOCAL 0，实例化不变式 Foo() 得实例）、入口
        // ModuleEntry 返回模块对象常量（主脚本与导入模块同规，IMPORT 栈效应的兑现），其余返回 nil
        // （显式 return 后为死代码，无害）。kind 读 cur_fn_ctx()->kind_，须在目标上下文就位后调用。
        // loc 供模块对象常量的溢出报错定位；行号取 loc.line()。
        void emit_implicit_return(SourceLoc loc) const;

        // name 为函数名 StringView（具名 fun 声明名 / lambda kAnonymousName / 入口 `<main>` / 类成员方法
        // 名）。name 建串与守卫收口在工厂 StringView 重载内（工厂守「自己创建的」）。kind 决定 CLOSURE
        // 后的绑定/注册分派（收口 bind_function_value）与帧形态（实例方法族槽 0 = 具名局部 this）及隐式
        // 返回尾。完成后切回父上下文，函数值已在父序列压栈。decl_loc 供 validate_params 报错与声明区
        // 发射行号，隐式返回尾 loc 取 body.loc()。无默认值。
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
