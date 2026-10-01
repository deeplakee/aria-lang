---
name: aria-compile
description: aria 解释器 compile 层模块参考：Token/Lexer/ast/Parser/AstVisitor/FnKind/FunctionCtx/ModuleCtx/CodeGen（单遍合一字节码编译器）/Compiler 编排器。读写 src/compile/** 或改文法（docs/grammar.txt）、词法/语法/语义检查、字节码发射时使用。
paths:
  - "src/compile/**"
---

# compile 层模块参考

Lexer / Parser 的实现应与 `docs/grammar.txt`（语言文法规范，留在 `docs/`）保持一致，改文法时同步更新相关代码与测试。

## `compile/TokenType.hpp`

- `TokenType`（覆盖文法全部终结符；关键字以 `ARIA_TOKEN_LIST` 第三列 `true` 行当次实测为准，当前 22 个）+ 可读名表 `kTokenNames` + 拼写表 `kTokenLexemes`（表项 `Pair<StringView, bool>`：固定拼写 + 是否关键字；关键字/运算符/标点/下划线为字面拼写，字面量与 EOF 记空串）+ `lookup_keyword`（顺序扫关键字行，大小写敏感）。枚举/名字表/拼写表由 `ARIA_TOKEN_LIST(X)` 全量注册表（X-Macro 单一事实源，条目 `X(名字, 拼写, 是否关键字)` 三列）同源展开，新增类型加一行即收口、名字串经 `#` 派生；风格对齐 `code.hpp` 的 `ARIA_OPCODE_LIST` 先例（注册表宏顶格、条目宏在展开点前 define/随行 undef，逐值注释用块注释因行注释会吞续行符）。

## `compile/Token.hpp`

`Token`（type + lexeme(`StringView`) + loc + `TokenValue`，工厂 `make_integer`/`make_float`/`make_string`）；`TokenValue = variant<monostate,i64,f64,String>`。

## `compile/Lexer.hpp`

- `Lexer::tokenize(SourceFile&) -> Result<List<Token>, List<Error>>`（静态入口、内部一次性构造）。**错误恢复式**：词法错误记入 `List<Error>` 后推进继续扫，达 `kMaxErrors`（= 32）置 `is_fatal_` 停止；无错才返回 token 流（含末尾 Eof）。`src_` 以 `\0` 结尾可作哨兵。
- 位置只记起点字节偏移（行列由 `SourceLoc` 在消费点派生，成本契约见 `util.md`）--词法期不维护行列计数，故游标推进就是普通字节推进、回退只需还原 `pos_`。
- 连续消费收三个逐层加约束的辅助：`consume_u8`（裸字节推进，**不保证游标停在码点边界**，只在停点必为 ASCII 时用）、`consume_ascii`（加一道 ASCII 守卫）、`consume_codepoints`（逐码点解码后整码点一步）。
- 性能基线与已实测否决的优化见 `.claude/reference/compile/lexer-notes.md`（基准入口 `bench/lexer_bench.cpp`，动词法性能前先读）。

## `compile/ast.hpp`

- AST 节点层次（`ASTNode` 根 -> `StmtNode`/`ExprNode`/`PatternNode` 分类基 -> 各具体节点；`ProgramNode : ASTNode` 为根的直接子节点，持顶层声明列表）；`ASTNode` **不可拷贝、不可移动**（节点总经 `UPtr` 堆分配，移动的只是指针）；运算符枚举 `Op::Binary`/`Op::Unary`/`Op::Assignment`（收口于 `namespace Op`，与 `TokenType` 解耦）；辅助值类型 `Param`/`MatchPattern`/`MatchArm`/`MatchExprArm`/`VarBinding`/`MapEntry` 等；节点持 `SourceLoc`，`line()` 直返 `loc_.line()`（无 0->1 兜底）。
- `dump(indent)`/`display()` 渲染树形文本：节点 dump 统一经 `detail::ast::dump_node(indent, header, kids...)` 变参收口，子项按 UPtr 空安全/值/List 三族泛型 `dump_child` 自动分派（基础设施与 `write_line` 同在 ast.hpp）；形态特例（TryStmtNode catch 子树 indent+2、MatchPattern wildcard 早返回、条件拼 header）保留手写。每个具体节点有 `accept(AstVisitor&)` override（双分派）。

## `compile/Parser.hpp`

`Parser::parse(List<Token>) -> Result<UPtr<ProgramNode>, List<Error>>`（静态入口、内部一次性构造）。递归下降、各函数与文法非终结符一一对应；内部用 `AriaCompileException` 在递归下降深处传播语法错误，`declaration()` 层捕获 + panic-mode `synchronize()` 恢复式收集（同 Lexer）。游标辅助 `peek`/`check`/`match`/`advance`/`previous`/`expect`/`expect_identifier`/`synchronize`；for/for-in、fun 声明/lambda、`{` block/mapExpr 等按调用上下文消歧。

## `compile/AstVisitor.hpp`

AST 访问者接口，作为代码生成阶段「字节码编译器」等遍历类的抽象父类：每个具体节点一个 `visitXxxNode(XxxNode&)` 纯虚（子类须逐一 override，编译器据此强制覆盖全部节点类型，避免漏处理），双分派由节点的 `accept(AstVisitor&)` 完成。本头只前置声明各节点类型（与 `ast.hpp` 解耦），子类需自行 include `ast.hpp`；参数用非 const 引用。具体子类：`CodeGen`。

## `compile/FnKind.hpp`

函数种类独立小头（AST 侧与编译执行侧共用）：`FnKind`（Function/Lambda/StaticMethod/Method/InitMethod/ModuleEntry）+ `is_method`（实例方法族判据）。各 kind 的绑定形态/隐式返回尾/槽 0 语义差异的权威表述见该头注册表注；ModuleEntry = 模块入口体（主脚本与导入模块同规，`ModuleCtx` 构造直接烙定，不经 `compile_function`，故 `bind_function_value` 对其 UNREACHABLE）：返回尾恒压模块对象常量、体顶层带值 return 编译期拒绝（`ReturnValueAtTopLevel`，裸 `return;` = 提前退出与隐式收尾同形）。

## `compile/FunctionCtx.hpp` / `.cpp`

单函数编译上下文，收口每函数可变状态：**本类只负责「登记」**（局部/作用域/循环栈/upvalue 捕获描述/常量池索引），「发射」仍由 CodeGen 负责。

- **单构造 `explicit FunctionCtx(ObjFunction* fn, FunctionCtx* enclosing, FnKind kind)`**（kind 无默认值；enclosing 空 = 入口，否则嵌套指向外层）。**所有权**：入口 fn 上下文由 `ModuleCtx` ctor `new`、dtor 沿 `enclosing_` 链 `delete`；子上下文由 `compile_function` `new`（成功路径 delete、出错交 `~ModuleCtx` 走链）。父编译期长于子，故 `enclosing_` 裸指针在子生命期内稳定。
- **字段与方法**：`locals_`（clox 风格局部栈，`locals_[0]` = 哑元 slot 0 = callee，1..A = 形参，A+1.. = 体局部）、`scope_depth_`、`loop_stack_`、`upvalues_`（捕获描述表，体编译期经 `add_upvalue` 登记、成功路径由 `compile_function` 尾部整表 flush 进 `fn->upvalue_descs()`）、`kind_`、`constant_index_`（**常量池去重索引** `HashMap<Value,u16>`，键相等用 `===`，走值层 std 特化）；`add_local`（压 `Local{name, scope_depth_, is_captured=false}` 返 slot，**纯登记**不发射/不查重）、`add_constant`（同值复用池内已有索引、未命中追加并登记，返池索引；池溢出不在本层判）、`add_upvalue`（同 `(is_local,index)` 去重复用；追加将越出 u8 索引域 (`size > kMaxUpvalues`) 返 `nullopt` 交 CodeGen `fail TooManyUpvalues`）、`is_defined_in_scope`/`find_local`、`begin_scope`/`end_scope`（退出块作用域时 `--scope_depth_` 后弹出原 scope 局部并真正移除登记；break/continue 不得走此--跳转后的语句仍在作用域内可引用这些局部，须保留登记）。
- **常量池去重索引为什么在这**：索引是**编译期草稿**（每个函数一份、随本上下文销毁；池本体 `fn_->unit().constants` 才是产物），故不放进 `CodeUnit`--否则每个函数对象常驻 40 字节壳与索引堆。不参与 GC（std 分配器）、不是 GC 根：键在池内都有同值副本保活。**每函数一份是硬约束**：共用一张表漏清一次就会从上一个函数的池里拿到索引，那是静默发射错常量。
- `FnKind` 见上 `compile/FnKind.hpp` 一节（this 解析「沿 ctx 链找最近实例方法」与 super 判据「当前帧为实例方法族」两判据消费 kind）；`kThisName = "this"`（this 是关键字不可能与用户标识符撞名，可安全作局部登记名）定义于本头。`LoopCtx` 字段语义与三种循环 + break/continue 的占位回填用法详见 `.claude/reference/compile/loopctx.md`。**循环上下文随函数走**：进新函数即得空 `loop_stack_`，break/continue 不会跨函数绑定外层循环。

## `compile/ModuleCtx.hpp` / `.cpp`

模块编译上下文，收口每模块状态：模块句柄 `module_` + 当前函数上下文游标 `current_fn_ctx_`（公有，**兼拥有入口 fn 上下文**）+ 私有顶层全局名注册表 `defined_globals_`（按名字内容判等）；与 `FunctionCtx` 对齐成「模块 > 函数 > 作用域」三层。

- **`current_fn_ctx_` 单成员兼两职**：既是入口 fn 上下文所有者（ctor 分配 / dtor 释放），又是「当前编译到哪个函数」的游标--子上下文由 `compile_function` `new`、enclosing_ 回父、游标摆向子；**成功**路径还原游标（`= child->enclosing_`）并手动 `delete child`，**出错**路径 `fail()` 抛 `AriaCompileException` unwind（不还原/不 delete 子，见 CodeGen），故无需 owner + cursor 两指针。「当前 CodeUnit」不再单独存，由 CodeGen 经 `cur_cu()` 派生。entry 判定 = `current_fn_ctx_->enclosing_ == nullptr`。
- **析构走链**：`~ModuleCtx`（定义于 .cpp，需 `FunctionCtx` 完整类型）沿 enclosing_ 链从 `current_fn_ctx_` 走到 entry 逐个 `delete`，无论游标在哪儿都对--异常 unwind 保证出错后无 `cur_cu()`/`cur_fn_ctx()` 访问，故无需「游标必回入口」不变式。构造函数（.cpp）**要求调用方先 `module.set_entry(entry)`**（ASSERT entry 非空）并自建入口 fn 上下文。一次性实例、不复用、无 `reset`，由调用方以 `UPtr<ModuleCtx>` 持有；copy/move 删除。`gc_` 留 `CodeGen` 不入此。
- **轻封装**：`module_`/`current_fn_ctx_` 公有（CodeGen 直接读写），`defined_globals_` 私有（仅经内联 `declare_global` 访问，true = 新登记 / false 已存在 -> 调用方 `fail RedefinedVariable`）；`is_global_scope() const noexcept`（定义于 .cpp，收口「当前是否模块顶层作用域」判定，由 `bind_stack_value` 直接消费）。首错经 `AriaCompileException` 抛出即 unwind（CodeGen 无 `error_` 成员），天然「整个 pass 的第一个错停住」。

## `compile/CodeGen.hpp` / `.cpp`

字节码代码生成器 `CodeGen final : AstVisitor`，单遍合一（clox 风格）--一次 AST 遍历同时做名字解析 + 语义检查 + 字节码发射，把 `ProgramNode` 编译为模块入口 `ObjFunction*`（arity 0、名由 `compile` 的 `entry_name` 参数定：主入口 `<main>`、运行期导入模块 `<module>`）。

### 状态分离与游标

- 每函数可变状态交 `FunctionCtx`，每模块状态（模块句柄 + 当前函数上下文游标 + 顶层全局名注册表）交 `ModuleCtx`，形成「模块 > 函数 > 作用域」三层；「当前函数」不作 CodeGen 成员--游标 `current_fn_ctx_` 寄存于 `ModuleCtx`（公有），CodeGen 经内联 `cur_fn_ctx()` 只读访问、`compile_function` 经 `mod_ctx_->current_fn_ctx_ = ...` 摆动；「当前 CodeUnit」由 `cur_cu()`（= `&cur_fn_ctx()->fn_->unit()`，.cpp 定义需 ObjFunction 完整类型）派生，随游标自动切换（免两指针同步 save/restore）。CodeGen 持 `UPtr<ModuleCtx> mod_ctx_`。

### emit 解耦与失败翻译

- 字节码编码逻辑（`emit_op`/`emit_byte`/`emit_word`/`emit_pop_n`/`emit_jump`/`patch_jump`/`emit_jump_back`/`emit_load_local`/`emit_store_local`/`size`）**下沉 CodeUnit**，CodeGen 不再自持这些薄包装；发射目标由 `cur_cu()` 决定，越界（跳转偏移/回边超 64KB）由 CodeUnit 方法返 bool、CodeGen 翻译为 Error。
- **单层 `_or_fail`（操作 + 失败即 fail）**：`add_constant_or_fail`（溢出 `> kMaxConstants` -> `fail CodeUnitTooLarge`）/`add_name_or_fail`（`new_string` intern 后入池，无守卫）/`define_local_or_fail`（同 scope 重名 -> `fail RedefinedVariable`、`> kMaxLocals` -> `fail TooManyLocals`）/`define_global_or_fail`（全局重名 -> `fail RedefinedVariable`，判重后发 `DEF_GLOBAL` 弹值定义）/`add_upvalue_or_fail`（越出 u8 upvalue 索引域 -> `fail TooManyUpvalues`）/`resolve_name_or_fail`（局部 -> Local / 外层 -> Upvalue / 否则 Global）供 visit 层（有节点 loc）调用，失败即 `fail(...)`（`[[noreturn]]`，之后值恒有效）并返回解包值；`patch_jump_or_fail`/`emit_jump_back_or_fail` 底层返 bool、无解包值，故 void 封装仅翻译失败。
- **声明绑定收口 `bind_stack_value(name,loc)`**（值已在栈顶的声明名：全局 -> declare 判重 + `DEF_GLOBAL` 弹值定义 / 局部 -> 值填槽 declare；var/fun/def/import 四处共用，var 的初始化器先于绑定求值）。

### 值填槽不变式

- `define_local_or_fail` 仅 `add_local` 登记（不发 `LOAD_NIL`，非 clox 预占槽），登记即初始化、无独立 init 状态。**关键不变式**：声明名登记时值已压栈，「下个局部 slot = 当前栈高」，初始化器/`LOAD_NIL`/`LOAD_CONST` 值恰好压在 slot 成为该局部，无需 store/pop。for-in `<iter>`/pattern 亦用值填槽，无例外；形参 caller 压栈后登记。
- var 的初始化器先于声明名求值（init 不登记当前帧局部，init 里的同名引用沿 resolve 链落外层：遮蔽场合捕获外层、落全局运行期 `UndefinedVariable`）。

### 上下文所有权与错误通道

- 入口 fn 上下文由 `current_fn_ctx_` 自拥有，`compile_function` 成功路径删子并还原游标、出错路径交 `~ModuleCtx` 走链释放。**错误通道**与 Parser 同：编译期深层 `fail()` 抛 `AriaCompileException`（持 `Error`，`[[noreturn]]`）自动 unwind 跨 visit 递归栈，`compile()` 顶层 `catch` 翻译为 `Result`--无需 `error_` 成员 / `ok()` / 各 visit 的 `if (!ok()) return` 守卫，首个错误自然即止。
- **`compile(GC& gc, ProgramNode&, ObjModule*, StringView entry_name) -> Result<ObjFunction*, Error>`**（静态入口，`entry_name` 无默认值）：入口 `gc_.make_guard(module)` 把 module 入临时根贯穿全程（经 `module.entry_ -> 常量池 -> 嵌套 fn 常量池 -> ...` 整链根化建设中 ObjFunction/常量池 ObjString）；每个子 fn 在 `compile_function` 起始即 `add_constant_or_fail` 入父常量池（先于编译体），入池即经 module 根链可达；`new_object -> add_constant` 间走 trivial 分配不触 GC，无需守卫。调 `init_module`（建入口函数 + `module.set_entry` + `make_unique<ModuleCtx>`），随后 `try { 逐顶层声明 emit_stmt + emit_implicit_return（入口 ModuleEntry：压模块对象常量 + RETURN；函数体其余 kind：InitMethod 返 this / 其余返 nil） } catch (AriaCompileException& e) { return std::unexpected(e.error()); }`，`~CodeGen` 自动释放 `mod_ctx_`。

### 栈契约与行号线程化

- **栈契约**：ExprNode 子类留一值、StmtNode 子类留零值；父 visit 显式调 `emit_expr`/`emit_stmt` 编排子节点（出错由 `fail()` 抛异常 unwind，无需逐调用短路）；所有底层 emit 经 `cur_cu()->emit_*`。
- **行号线程化**（不持 `line_` 成员）：行号是「当前发射节点的行」属每节点局部状态，故每个直接发射的 visit 持局部 `const u32 line = node.line()` 并传给 emit（尊重 CodeUnit「emit 一律带 line、无状态」设计）；`bind_pattern(pat)`/`compile_function(...)` 就地取 `pat.line()`/`body.line()`，`define_local_or_fail` 纯登记不带 line。仅编排子节点的 visit 不持 `line` 局部。

### 作用域模型与裸名解析

作用域模型（Python/JS）：

- `mod_ctx_->is_global_scope()`（当前函数为入口且第 0 层 scope）的 `var`/`fun`/`import` 别名 -> 模块全局（`DEF_GLOBAL` / `IMPORT` + `define_global_or_fail` 内容判重重定义检查）；嵌套块/函数内 -> 局部（`define_local_or_fail` 重名 -> `fail RedefinedVariable`、溢出 -> `fail TooManyLocals`，否则纯登记、不发指令）。有初始化器：`emit_expr(init)` 先求值，值恰好压在 slot；无初始化器：发 `LOAD_NIL` 填槽。无需预扫。
- 全局**不做编译期 init 追踪**（支持前向引用如互递归，定义与否属运行期属性），故全局自引用 `var x = x + 1` 在初始化器里 `LOAD_GLOBAL x`（此时 `DEF_GLOBAL x` 未执行）-> 运行期 `UndefinedVariable`（同型语义：函数体局部 var 的 init 里的同名引用也落此外层）。
- **裸名解析** `resolve_name_or_fail(name, loc) -> ResolvedVar` 序「局部 -> upvalue -> 全局」：当前函数局部 -> Local；外层 -> Upvalue（`resolve_upvalue(ctx, name, loc)` 递归（clox resolveUpvalue 形）--直接外层命中 -> 置该局部 `is_captured=true` + `add_upvalue({is_local=true, slot})`；未命中 -> 递归穿透捕获 -> `add_upvalue({is_local=false, 外层视角 upvalue 索引})`；到 entry 之上无外层 -> 返 `nullopt` 落 Global。`add_upvalue` 同 `(is_local,index)` 去重复用；登记经 `add_upvalue_or_fail`（越界 -> `fail TooManyUpvalues`，nullopt 不外泄免被误读为「落全局」））；否则 -> Global（运行期查模块全局表，未定义报 `UndefinedVariable`）。解析序与 grammar.txt 既定一致。

### 函数编译 `compile_function(name, params, body, decl_loc, kind)`

FunDecl/Lambda/类成员方法共用；kind 无默认值、调用处显式写明（`visitFunDeclNode` 按节点 `FnKind` 三态、`visitLambdaExprNode` 传 Lambda）；name 恒非空（具名 fun 为声明名、lambda 为 `<anonymous>`、入口为 `<main>`、类成员方法为成员名），name 串由工厂 StringView 重载 intern 并自守。

- **参数合法性检查抽成 `validate_params(params, loc)`**（compile_function 体首调用；只读 params、不触碰编译器状态，首错即 fail）：形参重名 -> `DuplicateParam`、> 255 形参 -> `TooManyParameters`，loc 取声明节点（fun 关键字）。**成员不查重**：类体成员各自 `accept` 分派后直接落类表（与体外 `Foo.x = v` 同形态），重名**后写遮蔽**、不报错（见 grammar.txt）。
- **绑定分派**（收口在 `bind_function_value` 的穷尽 switch）：方法三态留栈不绑定、就地注册 `MAKE_STATIC`/`MAKE_METHOD` 消费；Lambda 留栈作表达式值（名恒 `kAnonymousName`）；具名 fun 绑定全局（顶层 `DEF_GLOBAL`）/局部（嵌套 `define_local_or_fail` + `CLOSURE`，值填槽）。建子函数经 `new_function` 后直接 `add_constant_or_fail` 入父常量池（**无守卫**：`add_constant -> constants.push -> reallocate<T>` 走 trivial 分配不触 GC）+ 父发 `CLOSURE fn_idx`（描述表在体编译后 flush 进 `fn->upvalue_descs()`，存 `ObjFunction` 元数据、不在字节码流）；建子上下文 `new FunctionCtx{fn, cur_fn_ctx(), kind}` + 摆动游标。成功收尾在子 unit 发隐式 return（**InitMethod 尾 = `LOAD_LOCAL 0; RETURN` 返回 this**、其余 = `LOAD_NIL; RETURN`）+ flush upvalues + 游标摆回父 + `delete child`；出错则 `fail()` unwind 跳过，子留链交 `~ModuleCtx`。
- **缺省参数序言**（印章方案）：与参数登记单循环交错、按声明序--先逐缺省槽发 `LOAD_LOCAL s; LOAD_REG DefaultMark; EQUAL; JUMP_FALSE 跳过; <默认值表达式>; STORE_LOCAL s; 跳过:`（运行期 `call_closure` 已把未传槽 `[argc+1..n]` 垫充 DefaultMark 印章并补齐满参栈深；默认值仅未传时求值、按声明序从左到右补）、后 `add_local` 登记本参数名--前序参数已登记可被缺省表达式引用，自身/后序参数名未登记、对解析结构性不可见、按常规解析链落外层/全局（Python/C++ 默认值作用域同款）。
- **import 按作用域绑定**：`visitImportStmtNode` 按 `is_global_scope()` 分派，与 `var`/`fun` 同形 lowering（`IMPORT` 仅压模块值于栈顶，绑定由 CodeGen 走：顶层 -> `define_global_or_fail(alias)` 判重 + `DEF_GLOBAL` 弹值；嵌套 -> `define_local_or_fail(alias)` 值填槽，无 `STORE_LOCAL`）。
- **GC 根纪律（启用 GC 后）**：`new_string` 返回的 name 串是 weak root（intern 不保命），裸持跨任何可能触发 `maybe_collect` 的子编译/分配即可能被扫--故 name 建串与守卫收口在工厂 StringView 重载内。这些守卫保护的是「跨 `new_object`/`new_function`/`emit_expr` 等**真 `maybe_collect` 触发点**」的窗口；反之「fresh 对象裸持跨一次 trivial 分配（`add_constant` 的 `constants.push`/`intern_insert` 的 `allocate`）再发布进结构」的窗口**不需要守卫**（trivial 分配永不触发 GC，`GC.hpp` 核心不变式），故 `add_name_or_fail` 的 `new_string`、`compile_function` 的 `new_function` 结果均立即入池。

### 跳转回填（编码在 CodeUnit）

- `cur_cu()->emit_jump` 返回占位偏移 src_off；`patch_jump(src_off)` 前向 `target_off - base_off`（base_off = 读完操作数后的 ip）；`emit_jump_back(target_off)` 后向（越界返 false -> CodeGen 翻译）。`||`/`&&` 短路（`JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP`）发射后须 `patch_jump` 到 rhs 之后（L_end）；for 的 continue 前向回填须在递增发射**前**回填（否则错跳 L_end 提前出循环）；break/continue 先 `emit_pop_locals_to(loop.loop_scope_depth)`。

### 弹区清理与 `CLOSE_UPVALUE` 发射

- 退出作用域与 break/continue 共用同一发射口 `emit_pop_locals_to(target, line)`--自栈顶（最内）向外遍历 depth > target 的局部尾段，整区一条 `POP_N`（被捕获局部一并计数），弹区含被捕获局部才追加一条 `CLOSE_UPVALUE`（对齐 Lua `OP_CLOSE`）。只发射不改登记：`end_scope`（块/for/for-in/try）= 先 `emit_pop_locals_to(scope_depth_ - 1)` 再 `FunctionCtx::end_scope()` 收尾（登记移除唯一出口）；break/continue 直接调 `emit_pop_locals_to(loop.loop_scope_depth)`、登记保留。for-in per-iteration 出口经此获得每轮新鲜绑定语义。

### for-in lowering（值填槽，无预占/peek-store）

`<iter>` 与 pattern 均值填槽。

- `<iter>` 在 for-in scope（循环全程）：`emit_expr(iterable)` 后经 `emit_method_call0("iter", loc)`（封装 `PREPARE_METHOD name` + `CALL_METHOD 0` 的 0 参方法调用原语，receiver 由调用方先压栈）出值后 `define_local_or_fail("<iter>", loc)` 值填槽（无 `LOAD_NIL`/`STORE_LOCAL`/`POP`）。
- 每轮开 per-iteration scope：`LOAD_LOCAL <iter>` 后 `emit_method_call0("has_next")` / `JUMP_FALSE L_end`，再 `emit_method_call0("next")` 出值后 `bind_pattern(pattern, Fill)` / 体 / per-iter `end_scope` / `JUMP_BACK`；`L_end` 后 for-in `end_scope` 弹 `<iter>`。
- iter/has_next/next 经 `PREPARE_METHOD`+`CALL_METHOD` 两段式派发（解析经 `Object::load_field`，用户定义类与内建 list/map/string/range 同走一路，降糖对来源不可区分）。`visitCallNode` 另对 `recv.name(args)` 形态（含 `this.name(args)`，收口于 `try_emit_method_call`）统一发两段式，`super.m(args)`（`SuperExprNode`）与下标调用 `arr[i](args)` 保持 `LOAD_SUPER_FIELD`/`LOAD_INDEX` + `CALL`。

### 复合赋值与 lvalue 模式

见 `.claude/reference/compile/compound-assignment-lowering.md`。load/store 发射收归 `visitIdentifierNode`，经上下文 flag `LvalueMode{Load,Prepare,Store,Locate}`（CodeGen 成员 `lvalue_mode_`，默认 `Load`）告诉节点当前作为读/定位准备/写哪条腿。

- `emit_lvalue(node, mode)` 设置 `lvalue_mode_` 后 `n.accept` 分派（不在分派后恢复）；目标节点入口经 `take_lvalue_mode()` 一次性 take（取值并清空为 `Load`），故子节点经 `emit_expr` 时 flag 已清空、不泄漏。`emit_expr` 入口 `ASSERT(lvalue_mode_ == Load)` 在开发期捕获漏 take 的 bug。
- `validate_lvalue_target`（dynamic_cast 守卫）放行 Identifier/Field/Index 三种合法左值种类、其余 `InvalidAssignmentTarget`；在 `emit_lvalue` 分派前调用（两种赋值首腿均先于 rhs 抛错）。**普通 `=`** = `Prepare` + value + `Store`（Prepare 只发接收者，receiver-first 发射权在节点 Prepare 臂，Identifier/帧内 this 恒 no-op；peek-store）；**复合** = `Locate` + value + op + `Store`；**前置 `++`/`--`** = `Locate` + `LOAD_IMM 1` + `ADD`/`SUBTRACT` + `Store`。
- `Locate` 已启用：一般对象/捕获 this 定位腿发 `<obj> DUP LOAD_FIELD f`，DUP 副本留 VM 栈、Store 腿复用（非编译期 stash，见 compound-assignment-lowering.md §4.2；IndexAccess 定位腿的 DUP2 形态亦已接线，§4.3）。

### match 发射（纯降糖，零新指令）

- 两 visit 一行委派模板总口 `emit_match`（`validate_match_arms` 折入总口开头；臂体经 `emit_arm_body` 重载分派）--subject 求值一次驻留栈上（in-flight 临时，无隐藏局部；命中臂入口 `POP` 消费，全臂未命中由 `THROW` 的 unwind 清栈）。
- 逐臂展开「`DUP` 副本 + pattern 表达式 + `EQUAL`（= value_equal）+ `JUMP_FALSE` 未命中跳下臂（占位回填下臂起点，末臂回填兜底）」，`_` 臂无比较直入（恒末臂，其后死臂经模板辅助 `validate_match_arms` 编译期拒绝），臂体后 `JUMP L_end`（越兜底）。
- 全臂未命中发 `LOAD_REG MatchNoArm`（寄存器共享 ObjException，bootstrap 铸造、消息静态）+ `THROW`；语句臂走 `emit_stmt`（净零值）、表达式臂走 `emit_expr`（每臂恰一值）；pattern 按臂序惰性求值。

### try/catch/throw 发射

VM 侧语义见 `.claude/reference/runtime/exception-implementation-pitfalls.md`（坑 #4/#10）与 `runtime.md`「VM 异常通道」。

- `visitThrowStmtNode` = `emit_expr(e)` + `THROW`（弹值入寄存器，unwind 派发时值落 catch 参数槽）。
- `visitTryStmtNode`（仅 try/catch；`finally` 不支持、善后 defer 为可选后续；无 catch -> `fail TryWithoutHandler`）lowering 采用**入口预插占位 + 结尾回填**：入口快照 `stack_depth = cur_fn_ctx()->locals_.size()`（try 体 scope 开前）+ `begin = cur_cu()->size()` + `try_records.push` 占位 -> `begin_scope` + 编译 try 体（嵌套 try 在此各自预插，记录按 begin 非降序）+ `end_scope` -> `end = size()` + `emit_jump`（正常路径跳过 catch）-> L_catch `begin_scope` + `define_local_or_fail(catch_param)`（**无 STORE_LOCAL**--e 槽恒 == `stack_depth`，unwind 截栈后 push 恰落该槽，值填槽不变式）+ 编译 catch 体 + `end_scope` -> `patch_jump` -> 回填占位项 `end/handle/stack_depth`。
- 栈平衡：try 体 `end_scope` 与 catch 子句 `end_scope`（弹 e + catch 体局部）都回到 `stack_depth`，两路径在 L_end 齐平。

### M5 类发射

VM 机制见 `runtime.md`。

- **`visitDefDeclNode` lowering**（成员即表写入，与体外 `Foo.x = v` 同形态，重名后写遮蔽不查重）：① superclass 有 -> `resolve_name_or_fail` + `emit_load_var`（运行期解析 superclass 值，编译期不查全局）/ 无 -> `LOAD_REG`（寄存器 `ObjectClass`，用户 shadow 免疫）② `MAKE_CLASS name` ③ 类名绑定先于体编译（收口 `bind_class_value`，三腿分派镜像 `bind_function_value`；对齐 clox `classDeclaration` 的 OP_CLASS+OP_DEFINE_GLOBAL 先于类体与函数先例）：全局腿 -> `DUP` + `define_global_or_fail`（DUP 副本弹值提前入全局，原类值驻栈贯穿类体；构建窗口内裸名经全局解析到构建中类对象，静态初始化器 throw 后类名保持绑定 = 残留语义，对齐 Ruby；重定义仍由编译期注册表兜底）/ 函数/块内 -> `define_local_or_fail`（值填槽，类值恰在 `locals_.size()` 槽位；unwind 截栈槽与半成品同弃，不留可见绑定）/ 成员位（嵌套类，`DefDeclNode.is_member`，parser 成员分派 def 腿递归烙定）-> `DUP2` 对复制 + `MAKE_STATIC` + `POP`（栈 `[enclosing, class]` 复制栈顶对令 `peek(1)`=enclosing 挂进外层类静态表、POP 弃 klass 副本恢复体前栈形；DUP2 对复制惯用法同 IndexAccess 定位腿；体内自引用经全路径 `A.B`，裸名不解析，文法无 enclosing 链）④ 成员按源序发射（静态变量初始化顺序即此序，前一静态可被后续初始化器引用）⑤ 尾部 `POP`（`is_member || is_global_scope()`：全局腿 DEF_GLOBAL 弹的是 DUP 副本、成员腿 MAKE_STATIC 弹的是值副本，驻留原值都在尾部归位；局部腿无尾，类值即局部、随作用域收尾弹出）。
- **this/super 解析**：`visitThisExprNode` -> `resolve_this_or_fail`（沿 fn ctx 链找 `kThisName` 的局部--当前帧命中 -> Local 恒槽 0；外层命中 -> `resolve_upvalue` 捕获（arrow 语义，穿透多层）；链上无实例方法 -> `fail ThisOutsideClass`；**永不落全局**）。`visitSuperExprNode`（`super.成员` 文法单形，裸 super 解析期 `ExpectedToken`）-> 语境检查（`SuperOutsideMethod`）+ `LOAD_SUPER_FIELD`（方法闭包绑 this、静态槽原值直读）；super 写形态无对应语义 -> `validate_lvalue_target` 拒绝报 `InvalidAssignmentTarget`。`super.m(args)` 经 visitCallNode 通用路径复用本 visit。
  - **this/super 的不对称判据**：this 允许嵌套捕获而 super 禁止。this 是帧槽 0 的具名局部，栈槽值可 upvalue 化；super 是
    `(defining class, this)` 二元组，而 defining class 挂在闭包上、不是局部，无槽可捕。故 super 仅直接方法帧可用，
    嵌套函数内需先取后用（`var m = super.m;` 语义等价，表达力无损）。
  - **否决的替代路线**：Wren 式 superclass 常量池烘焙（方法绑定时刻扫字节码回填 SUPER 常量槽、`CODE_CLOSURE`
    递归进嵌套 fn）机制成立且支持嵌套 super，但绑定时刻回填字节码过于 tricky，且表达力经先取后用无损，不值
    `LOAD_SUPER_FIELD` 加常量操作数 + `MAKE_METHOD` 回填扫描的改动面；defining class 戳复制给嵌套闭包亦否--
    戳一职双任（super 来源 + 方法性标记），嵌套闭包带戳会被 `is_method` 误判为方法性。
- **`visitFieldAccessNode` 四模式**（take 入口取）：object 为 this 且 `is_in_method()`（当前帧直接方法帧，槽 0 即具名 this）-> `LOAD_THIS_FIELD`/`STORE_THIS_FIELD`（this 取帧槽 0 不经栈，Store peek-store `[v]->[v]`；Prepare no-op；Locate 与 Load 同形）；其余退化/一般经栈：Prepare = 只发 `<obj>`（普通 = 首腿，不读值）；Load = `<obj>`（捕获 this 走 LOAD_UPVALUE）+ `LOAD_FIELD`；Store = **只发 `STORE_FIELD`**（`[obj, v] -> [v]`，接收者由 Prepare 腿备好）；Locate = `<obj> DUP LOAD_FIELD`（DUP 副本供 Store 腿复用）。普通 `=` 三腿与复合只在中间腿不同，接收者发射权在节点 Prepare 臂。
- **`visitIndexAccessNode` 四模式**（同款 take 入口取）：Load = `<obj>`+`<idx>`+`LOAD_INDEX`；Prepare（普通 = 首腿）= 只发 `<obj>`+`<idx>` 备对；Store = 只发 `STORE_INDEX`（obj/idx 由 Prepare 腿备好）；Locate = `<obj>`+`<idx>`+`DUP2`+`LOAD_INDEX` 复制对垫底供 Store 腿复用，locator 单次求值（compound-assignment-lowering.md §4.3）。
- **列表字面量发射**（`visitListExprNode`）：元素数先检后发（`> kMaxListElements` -> `fail TooManyElements`，`visitCallNode` 同款），逐元素 `emit_expr` 后 `MAKE_LIST n:u16`。

### 解构绑定

`PatternBindMode{Fill,Store}`（CodeGen 成员 `pattern_mode_`，默认 `Fill`）：`bind_pattern(pattern, mode)` 置模式后 `n.accept` 分派（模式对整个模式子树恒定，不 take、不还原），发射住三个 `visit*PatternNode`（栈顶值为待绑值）。

- **Fill**（`visitVarDeclNode` / for-in 目标）：`IdentifierPattern` -> `bind_stack_value`（顶层全局 `DEF_GLOBAL` 弹值、局部值填槽零指令）；`ListPattern` 逐位置压下标 + `LOAD_INDEX` 取元素后递归绑定，访问数 >= 2 时先 `define_local_or_fail` 隐藏局部 `<destructure_N>`（带槽号故同作用域唯一）复取源值--0 访问直弹源值、1 访问源值即消耗品不建隐藏局部；`_` 位置经 `is_wildcard_pattern` 过滤，不产生下标访问；rest 位（`...rest`）压「元素数」下标后 `MAKE_RANGE`（无上界位）作键取后缀。
- **Store**（`visitDestructureAssignmentNode`）：源值恒驻栈顶，每次访问前 `DUP`，`IdentifierPattern` 走 `resolve_name_or_fail` + `emit_store_var` + `POP`，收尾弹本层源值--净消耗栈顶一值，故调用点先 `DUP` 一份右值作表达式的值。`visitWildcardPatternNode` 恒 `POP`。

### 语义检查（首错即止，内联各 visit）

- `InvalidAssignmentTarget`/`BreakOutsideLoop`/`ContinueOutsideLoop`/`TryWithoutHandler`/`UnreachableArm`（match 通配臂后仍有臂，`_` 恒末臂至多一条）/`DuplicateParam`/`TooManyParameters`/`TooManyArguments`（> 255 实参）/`RedefinedVariable`（含 import 别名重定义）/`ThisOutsideClass`/`SuperOutsideMethod`/`NumberOutOfRange`（i48 范围）/`TooManyLocals`/`TooManyUpvalues`（单函数捕获将越出 u8 索引域：容量 = `kMaxUpvalues` + 1 = 256，第 257 条被拒）/`CodeUnitTooLarge`/`TooManyElements`（列表字面量元素数/map 字面量条目数超 u16 上限，先检后发）。顶层 `return` 合法（入口 `<main>` 亦为函数）。
- **varargs 为零发射翻转**：rest 即末位普通局部槽，值由 `call_closure` 打包就位，`compile_function` 传 `arity = 固定参数数 + is_varargs`、`min_arity` 遇 varargs 即止。43 个 `visitXxxNode` 全 override，全为真实发射（无占位）。

### 调试反汇编打印

`DEBUG_PRINT_COMPILED_CODE` 宏（CMake `ARIA_DEBUG_PRINT_CODE` option，OFF 默认，对齐 `ARIA_DEBUG_GC`/`DEBUG_LOG_GC`）控制是否在每个函数编译完成时打印其 CodeUnit 反汇编--`compile_function` 隐式 `RETURN` 后（游标仍在子）打印子函数、`generate_bytecode` 顶层 `try` 块后（游标仍在入口）打印入口；经 `io::println(stderr, ...)` 输出，关闭时 `#ifdef` 包裹、零开销。

### 测试 `tests/compile/test_codegen.cpp`

端到端 `tokenize -> parse -> CodeGen::compile -> AriaVM::run`，辅助 `run_source`/`compile_only` 以 `unique_ptr<AriaVM>` 持 GC 活到检视完返回值/反汇编（避免 vm 销毁致 ObjString 悬垂 use-after-free）。覆盖：嵌套函数内 break 不绑外层循环（循环上下文函数隔离）；M4 闭包（共享/引用/递归自捕获/unwind 幸存/写穿透 + 反汇编断言 CLOSURE 与 per-iteration CLOSE_UPVALUE + 容量下界钉子 256、去重只登记 1 条）；M5 类（实例化/继承 super/静态与字段遮蔽/this 捕获/动态成员/复合赋值字段腿 locator 单次求值 + 编译错 this/super 非法目标）；match 降糖链（首臂/`_` 兜底/MatchNoArm 共享身份/subject 单次求值/pattern 惰性）。

## `compile/Compiler.hpp` / `.cpp`

编译编排器 `Compiler`，收口「源文件 -> 可执行 `ObjFunction`」端到端链路。本身不做词法/语法/代码生成/磁盘 I/O--按序串联 `Lexer` -> `Parser` -> `CodeGen`，把三阶段 `Result<..., List<Error>>`（词法/语法，取 `errors_[0]`）/`Result<..., Error>`（CodeGen 已是单 Error）统一翻译为单个 `Result<ObjFunction*, Error>`（首错即止）。

- **吃实际的 `SourceFile`，不捏造源**：`compile()` 入参为 `SourceFile&`（调用方加载/构造的真实源文件），本类只读其 `content()` 做词法，不拥有、不重建源文件；无 `source_` 成员、无 `source_name` 参数。**生命期契约**：AST 节点持 `SourceLoc`（内含 `SourceFile*`），但 AST 仅 `compile()` 内部消费、不外返；失败返回的 `Error` 在构造期已把 `SourceLoc` 烘进自有 `message_`、不再持 `SourceFile*`，故 `source` 只须存活到 `compile()` 返回，调用方可立即释放/move。
- **静态服务/无状态**：`Lexer::tokenize`/`Parser::parse`/`CodeGen::compile(GC&, ...)`/`Compiler::compile(GC&, ...)` 均静态入口，无跨 `compile()` 复用。**GC 同源**：`compile` 的 `gc` 参数与 VM 同一 GC，编译期分配的 `ObjFunction`/`ObjString` 归此 GC、与后续 `run()` 同源；`CodeGen::compile` 入口 `make_guard(module)` 自守 module，故调用方无需为编译期再守模块。
- **接口** `compile(GC& gc, SourceFile& source, ObjModule* module, StringView entry_name) -> Result<ObjFunction*, Error>`（静态入口，`entry_name` 无默认值）：成功返入口 `ObjFunction*`（arity 0、名 `entry_name`，归属 gc、须在 gc 存活期使用）；失败返首错 `Error`。`entry_name` 透传给 `CodeGen`：主入口传 `kMainEntryName`（`<main>`）、运行期导入模块传 `kModuleEntryName`（`<module>`）。
- **与 `CodeGen` 分工**：`CodeGen` 是「AST -> CodeUnit（包在 `ObjFunction`）」代码生成器；`Compiler` 是「`SourceFile` -> AST」（`Lexer`/`Parser`）+「调 `CodeGen` 产出 `ObjFunction`」编排层，不做磁盘 I/O（加载源文件是解释器入口/REPL 职责）。

测试 `tests/compile/test_compiler.cpp`：经 `Compiler` 编排层端到端 `compile -> AriaVM::run`（非手拼），`SourceFile` 经 `unique_ptr` 持堆稳定地址活到检视完返回值/错误；含算术/递归/全局+while/var 自引用运行期 `UndefinedVariable`/语法错误，stress GC 锻炼编译期+运行期根接线。
