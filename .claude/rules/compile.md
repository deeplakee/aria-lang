---
name: aria-compile
description: aria 解释器 compile 层模块参考：Token/Lexer/ast/Parser/AstVisitor/FnKind/FunctionCtx/ModuleCtx/CodeGen（单遍合一字节码编译器）/Compiler 编排器。读写 src/compile/** 或改文法（docs/grammar.txt）、词法/语法/语义检查、字节码发射时使用。
paths:
  - "src/compile/**"
---

# compile 层模块参考

Lexer / Parser 的实现应与 `docs/grammar.txt`（语言文法规范，留在 `docs/`）保持一致，改文法时同步更新相关代码与测试。各文件的声明级契约以源码头注/注册表注为权威表述，本文件收模块定位、跨文件工作流与设计决策。

## 文件定位

- **TokenType.hpp**：终结符全量注册表，按类拆五段子表（special/literal/keyword/operator/punct），复合表 `ARIA_TOKEN_LIST(X)` 依序拼接（X-Macro 单一事实源，枚举/名字表两表同源展开、以枚举值为下标，风格对齐 `ARIA_OPCODE_LIST`；逐值注释用块注释因行注释会吞续行符）。关键字纯表 `kKeywords` 只由关键字子表展开（子表成员资格即判据，当前 22 个），配自纯表量出的长度区间早退 `kKeywordLenRange`，`lookup_keyword` 是唯一查表口。
- **Token.hpp / Token.cpp**：`Token`（type + lexeme 长度 u32（文本不落指针，读时按 loc 自源缓冲重建，词法期各 token 的 lexeme 区间恒以 loc 偏移为起点）+ loc + `TokenValue` 无 tag union（int_/float_/shape_，活跃成员由 TokenType 判别；字符串族不持内容，载荷 = `StringShape`（decoded_len + is_escape，打包与 8B 槽同宽）），平凡可拷贝），字面工厂（integer/float/string）+ 插值段三厂（interp_start/middle/end，均带 shape）与宽松取值；字符串族取值口 = `string_value()`（内层原文视图，剥引号/档边界，借自源缓冲）+ `shape()`。`Lexer::tokenize` 的返回即 `List<Token>`；解析内容不随流携带，解码在消费端（`util/str.hpp` 的 `decode_string_content`，CodeGen 字符串臂与 Parser import 路径共用）。
- **Lexer.hpp / Lexer.cpp**：`Lexer::tokenize(SourceFile&) -> Result<List<Token>, Error>`，**首错即止**（错误经 `AriaCompileException` 深处抛出、`tokenize` 顶层 catch 翻译为 Result，与 Parser/CodeGen 同族机制；报错点自陈位置：`error()` 同 `Parser::error` 形、位置锚 `start_`--dispatch 分派时=token 起点，`error_at(offset, ...)` 显式锚任意偏移，子扫描器如 `scan_escape` 入口重指 `start_` 换取更准列号且无需恢复）；位置只记起点字节偏移（行列由 SourceLoc 消费点派生，词法期不维护行列计数）。字符串即模板（无前缀，`${` 开档、裸 `$` 与 `{ }` 皆普通字符、`\$` 转义）：`scan_string` 驱动文本态/档内态两函数状态机交替，产 InterpStart/Middle/End 段 token，无档整串退化普通 String token；嵌套经 `dispatch_one` 递归（深度上限 `kMaxInterpDepth`=16）。转义集合校验全在词法期收口（`scan_escape`/`scan_unicode_escape`，首错即止；`\u` 的解析与校验收口 `util/str.hpp` 的 `decode_unicode_escape` 唯一解析口，失败以 `UnicodeEscapeError` 类别交还词法报错），字节产出延至消费端；扫描循环顺带记账每段展开后长度（`decoded_len`，简单转义计 1、`\u` 由码点区间映射 1-4）。性能基线与已实测否决的优化见 `.claude/reference/compile/lexer-notes.md`（基准入口 `bench/lexer_bench.cpp`，动词法性能前先读）。
- **ast.hpp / ast.cpp**：AST 节点层次（`ASTNode` 根 -> Stmt/Expr/Pattern 分类基 -> 具体节点，`UPtr` 堆分配）；名字成员（`IdentifierNode.name`/`Param.name`/`alias`/`ename` 等）一律 `StringView` 借自源缓冲（同 `StringLiteralNode.value`，存活至编译结束），仅在 codegen 构造 ObjString 时拷贝（收口 `add_name_or_fail`）；文法钉死的表达式位存具体节点（`ImportStmtNode.path` 指向 `StringLiteralNode`、`DefDeclNode.super` 指向 `IdentifierNode`），codegen 直接 `emit_expr`；运算符枚举 `Op::Binary/Unary/Assignment` 与 `TokenType` 解耦（映射收在 Parser.cpp）；dump 渲染经 `detail::ast::dump_node` 变参收口。节点族语义见各节点头注。
- **Parser.hpp / Parser.cpp**：`Parser::parse(List<Token>&) -> Result<UPtr<ProgramNode>, List<Error>>`（token 表借用不持有，存活须覆盖 parse 调用），递归下降各函数与文法非终结符一一对应；错误经 `AriaCompileException` 在递归深处抛出、`declaration()` 层 catch + panic-mode `synchronize()` 恢复式收集。消歧点（for/for-in 前瞻、fun 声明/lambda、`{` block/mapExpr 按调用上下文、解构赋值投机回退）的论证在各函数注释。
- **AstVisitor.hpp**：访问者接口，44 个 `visitXxxNode(XxxNode&)` 纯虚（1 根 + 17 语句 + 23 表达式 + 3 模式），编译器强制子类穷尽覆盖；双分派由节点 `accept` 完成。具体子类：`CodeGen`。
- **FnKind.hpp**：函数种类枚举（Function/Lambda/StaticMethod/Method/InitMethod/ModuleEntry）。各 kind 的绑定形态/隐式返回尾/槽 0 语义的权威表述见该头注册表注。
- **FunctionCtx.hpp / .cpp**：单函数编译上下文，**只负责「登记」**（局部/作用域/循环栈/upvalue 捕获描述/常量池去重索引），「发射」由 CodeGen 负责。所有权（入口归 ModuleCtx、子归 compile_function）与常量池去重索引「每函数一份是硬约束」的论证见头注。`LoopCtx` 字段语义与三种循环占位回填详见 `.claude/reference/compile/loopctx.md`。
- **ModuleCtx.hpp / .cpp**：模块编译上下文（模块句柄 + 当前函数上下文游标兼拥有入口 + 顶层全局名注册表），与 FunctionCtx 对齐成「模块 > 函数 > 作用域」三层；全局名注册表 `defined_globals_` 键借 `StringView`（AST 名字视图，存活覆盖编译期）；单成员兼两职的设计与出错析构走链见头注。
- **Compiler.hpp / Compiler.cpp**：编排器，串联 Lexer -> Parser -> CodeGen，统一翻译为 `Result<ObjFunction*, Error>`（首错即止）。吃实际 `SourceFile&`（不拥有不重建）；AST 不外返、Error 构造期已烘位置，故 source 只须存活到 `compile()` 返回。四入口（Lexer::tokenize / Parser::parse / CodeGen::compile / Compiler::compile）均静态无状态；compile 的 gc 与 VM 同一 GC。

## CodeGen（单遍合一）

`CodeGen final : AstVisitor`，一次 AST 遍历同时做名字解析 + 语义检查 + 字节码发射，产出模块入口 `ObjFunction*`（arity 0）。状态分离（模块/函数/作用域三层，游标寄存于 ModuleCtx）、emit 下沉 CodeUnit、`_or_fail` 失败翻译、值填槽不变式、裸名解析序（局部 -> upvalue -> 全局）、跳转回填、弹区清理、for-in/match/try/解构 lowering 的完整机制见 `CodeGen.hpp` 类头注与各方法注，及 `.claude/reference/compile/compound-assignment-lowering.md`（LvalueMode 与复合赋值）。

**设计决策（代码注释仅简版，论证在此）**：

- **全局不做编译期 init 追踪**（支持前向引用/互递归，定义与否属运行期属性）：全局自引用 `var x = x + 1` 在初始化器里 `LOAD_GLOBAL x`（`DEF_GLOBAL x` 未执行）-> 运行期 `UndefinedVariable`。
- **类成员不查重**：成员即表写入，与体外 `Foo.x = v` 同形态，重名后写遮蔽不报错（见 grammar.txt）；类名绑定先于体编译（自引用可用），静态初始化器 throw 后类名保持绑定 = 残留语义，对齐 Ruby。
- **this/super 不对称**：this 是帧槽 0 具名局部可 upvalue 化（arrow-function 语义）；super 是 `(defining class, this)` 二元组，defining class 挂闭包上无槽可捕，故仅直接方法帧可用、嵌套函数先取后用（`var m = super.m;` 语义等价）。否决的 Wren 式 superclass 常量池烘焙：绑定时刻回填字节码过于 tricky，且 defining class 戳复制给嵌套闭包会被 `is_method` 误判。
- **缺省参数作用域**：默认值表达式可见前序参数、自身/后序对解析结构性不可见（Python/C++ 同款）。
- **语言参照系**：值填槽是对 clox 预占槽模式的偏离；`CLOSE_UPVALUE` 对齐 Lua `OP_CLOSE`；复合赋值 locator-once 是 C 标准对 `E1 += E2` 的定义；`resolve_upvalue` 是 clox resolveUpvalue 形。

**语义检查（首错即止，内联各 visit）**：`InvalidAssignmentTarget`/`Break|ContinueOutsideLoop`/`TryWithoutHandler`/`UnreachableArm`/`DuplicateParam`/`TooManyParameters|Arguments`/`RedefinedVariable`/`ThisOutsideClass`/`SuperOutsideMethod`/`NumberOutOfRange`（i48）/`TooManyLocals`/`TooManyUpvalues`（容量 256 = `kMaxUpvalues`+1，第 257 条拒）/`CodeUnitTooLarge`/`TooManyElements`。顶层 `return` 合法；varargs 为零发射翻转（rest 即末位局部槽，值由 `call_closure` 打包就位）。

**坑编号纪律**：exception-implementation-pitfalls.md 与 coroutine-implementation-pitfalls.md 的坑编号空间各自独立（同号不同坑），src/ 注释引用坑号须带文档名前缀（如 `exception-pitfalls 坑 #4`）；exception 侧坑编号被广泛引用不得重排。

## 测试

- `tests/compile/test_codegen.cpp`：端到端 `tokenize -> parse -> CodeGen::compile -> AriaVM::run`；`run_value` 探针惯用法（顶层禁带值 return，包进 `__probe__` 函数从常量池取 ObjFunction 直跑）。覆盖闭包/类/match/解构/容量钉子。
- `tests/compile/test_compiler.cpp`：经 Compiler 编排层端到端（非手拼），stress GC 锻炼编译期根接线。
- `tests/language/` 语料（正向 assert/负向钉错误码）是改语言面的验收面。
