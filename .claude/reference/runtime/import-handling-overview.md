# 导入（import）端到端处理概览

> 本文介绍 aria 当前对 `import` 的**端到端处理**：从文法、词法、Parser、AST，到字节码、
> VM 执行、模块对象、文件系统原语，以及各环节的实现状态与测试覆盖。路径解析（裸名 / 相对 /
> 绝对规范键 / `.aria` 后缀 / 逐源根 exists-check / 模块表查表）的细节见
> [`import-path-resolution.md`](./import-path-resolution.md)，本文不重复，只在必要时引用。
> 指令格式与栈效应见 [`bytecode-instruction-set.md`](../bytecode/bytecode-instruction-set.md) §4.15。

## 一句话结论

导入路径处理目前已**完整实现**：「specifier -> 绝对规范键（磁盘解析）-> 模块表查重 -> 命中
复用并把 ObjModule 压栈（绑定交 CodeGen 按 DEF_GLOBAL / 值填槽走）」与未命中分支的「读盘 ->
编译（入口名 `<module>`，成功才入表）-> VM 内嵌套执行模块体（run-once）」整条链路均已在 VM
落地（加载事实源 = 模块表成员资格，对象无状态字段）。循环导入命中表内半初始化对象，被导入模块的
编译期/运行期错误原样透传（含其文件位置）。相对导入越界检测（`../` 越出源根）未做，留待后续。

## 端到端链路与状态

```
源码 import 语句
   │  ① 文法/词法            已定义 / 已实现
   ▼
Token (Import / As / String / Identifier)
   │  ② Parser              已实现
   ▼
ImportStmtNode { path:UPtr<ExprNode>(StringLiteralNode), alias:String }
   │  ③ AST→CodeUnit 编译器  ✓ 已实现（CodeGen : AstVisitor）
   ▼
OpCode::IMPORT  path:u16   (常量池 ObjString 索引; 压模块值于栈顶)
   │  ④ VM dispatch_loop() IMPORT 分支  已实现
   ▼
resolve_module()  →  new_string() intern  →  modules_ 查表
   (磁盘 exists-check + weakly_canonical)        │
        │                                          │
        │ 命中(体待跑/跑中/已跑完)                    │ 未命中(文件命中但模块未入表)
        ▼                                          ▼
  复用 ObjModule，压栈（绑定交 CodeGen 走）      ⑤ 加载层：磁盘读 + 编译 + run-once（load_module）
                                                   ✓ 已实现 -> 压栈
   解析失败(无源根命中) → ModuleNotFound
```

| 环节 | 状态 | 位置 |
| :--- | :--- | :--- |
| ① 文法 `import "str" as id;` | 已定义 | `docs/grammar.txt` 的 `importStmt` 产生式 |
| ① 词法关键字 `import` / `as` | 已实现 | `src/compile/TokenType.hpp`（`ARIA_TOKEN_KEYWORD_LIST` 的 `Import`/`As` 两行） |
| ② Parser 解析 import 语句 | 已实现 | `src/compile/Parser.cpp` 的 `import_stmt()` 与 `declaration()` 分派 |
| AST `ImportStmtNode` | 已实现 | `src/compile/ast.hpp`（节点）+ `ast.cpp`（`dump`/`accept`） |
| ③ AST→CodeUnit 编译器（发射 IMPORT） | 已实现 | `CodeGen : AstVisitor`，`visitImportStmtNode` 发 `IMPORT`+`DEF_GLOBAL`/值填槽 |
| `OpCode::IMPORT` 定义 | 已定义 | `src/bytecode/code.hpp` 的 `ARIA_OPCODE_LIST` |
| IMPORT 反汇编 | 已实现 | `src/bytecode/Disassembler.cpp` 的 `import_instruction` |
| ④ 路径解析 `resolve_module`（磁盘 + 绝对键） | 已实现 | `src/runtime/AriaVM.cpp`（匿名命名空间） |
| ④ IMPORT 命中分支（查表 + 压栈） | 已实现 | `src/runtime/AriaVM.cpp` `run_import` |
| ⑤ IMPORT 未命中分支（加载 + 编译 + run-once） | **已实现**（`load_module`：读盘 -> 编译 -> 成功才入表 -> `entry` 现场包空闭包经 `call_closure` 进帧交主循环 run-once） | `src/runtime/AriaVM.cpp` `load_module` |
| 源根列表 `source_roots_`（入口目录 + stdlib） | 已实现，run() 播种，**被 IMPORT 消费** | `src/runtime/AriaVM.hpp`（`source_roots()`/`set_source_roots`）、`AriaVM.cpp` 构造与 `run()` |
| `ObjModule` 对象 + `dir_`/`name_`/`abs_path()` | 已实现（`dir_` 指针恒非空、内容可空，`new_module` 默认 cwd；无加载状态字段，事实源 = 模块表成员资格） | `src/object/ObjModule.hpp`、`.cpp` |
| VM 模块表 `modules_` + GC 根 tracer | 已实现 | `src/runtime/AriaVM.hpp`（成员声明）、`AriaVM.cpp` 构造注册 tracer |
| fs 原语 `program_dir`（推导 stdlib） | 已实现，**被 VM 构造调用** | `src/util/fs.hpp` |
| fs 原语（read_file/absolute/current_dir） | 已实现，经 `SourceFile::from_path`（read_file）与 `fs::module_name_and_dir`（absolute）/`new_module` 默认目录（current_dir）进入加载链路 | `src/util/fs.hpp` |
| `SourceFile::from_path` | 已实现，**被 `load_module` 调用**（加载链路首步） | `src/util/source_file.hpp` |

## ① 文法与词法

- **文法**（`docs/grammar.txt` 的 `importStmt` 产生式）：`importStmt -> "import" string "as" identifier ";"`。
  强制字符串路径 + 强制 `as alias`，不支持裸名导入。
- **关键字**（`src/compile/TokenType.hpp` 的 `ARIA_TOKEN_KEYWORD_LIST`）：`TokenType::Import`、`TokenType::As`。

> 导入路径语义（源根列表与裸名/相对两类路径、目录包与源根越界的**未实现**状态、符号链接归一等）
> 以 `docs/grammar.txt`「说明」区的模块导入条与 [`import-path-resolution.md`](./import-path-resolution.md)
> 为准，本文不重复。

## ② Parser 与 AST

- **派发**（`src/compile/Parser.cpp`）：`declaration()` 的 `case TokenType::Import: return import_stmt();`。
- **`import_stmt()`**（`src/compile/Parser.cpp`）：`expect(Import)` → 期望
  `TokenType::String`（否则 `ErrorCode::ExpectedToken`「期望字符串字面量作为模块路径」）
  → 取 `advance()` 的 String token 经 `make_string_literal` 构造 `StringLiteralNode` 作 path
  → `expect(As)` → `expect_identifier()` 作 alias → `expect(Semicolon)` →
  构造 `ImportStmtNode(loc, path, alias)`。
  **path 文法钉死字符串字面量（Parser 保证指向 StringLiteralNode），此处不做任何路径解析或文件系统检查**。
- **AST 节点**（`src/compile/ast.hpp`）：`struct ImportStmtNode : StmtNode`，字段
  `UPtr<ExprNode> path;`（Parser 保证指向 `StringLiteralNode`）/ `String alias;`。
  `dump` 渲染 `ImportStmt as=...`，path 以 `StringLiteral` 子行渲染（`ast.cpp`），
  `accept` 调 `visitor.visitImportStmtNode(this)`。
- **visitor**（`src/compile/AstVisitor.hpp`）：`visitImportStmtNode` 为纯虚，由 `CodeGen` override。

> `CodeGen` 是 `AstVisitor` 的具体子类（`src/compile/CodeGen.hpp`），`visitImportStmtNode`
> 将 path（`static_cast` 收窄到 `StringLiteralNode`）的字面内容 intern 得常量索引，发射
> `IMPORT path:u16` 取模块对象压栈，再按作用域绑定（顶层 `DEF_GLOBAL alias` / 嵌套值填槽）。

## ③ 字节码

- **`OpCode::IMPORT`**（`src/bytecode/code.hpp` 的 `ARIA_OPCODE_LIST`）：枚举项，格式类别 `Import`。
- **指令格式**（`bytecode-instruction-set.md` §4.15）：`IMPORT path:u16`，栈效应
  `... -> [module]`（压模块值）。`path` 为常量池 `ObjString` 索引。绑定不在 IMPORT 内--交 CodeGen 按作用域经 `DEF_GLOBAL` / 值填槽走。import 不靠额外操作码。
- **反汇编**（`src/bytecode/Disassembler.cpp` 的 `import_instruction`）：读一个 u16 常量池索引，
  渲染 `IMPORT PPPP  ; path`。

## ④ VM 运行时

### 路径解析（磁盘 + 绝对键）

`resolve_module(StringView spec, StringView current_module_path, const List<String>& source_roots)`
定义于 `src/runtime/AriaVM.cpp`（匿名命名空间），返回 `Opt<String>`（命中文件的绝对
规范路径），由调用方经 `new_string` intern 驻留。要点：

- `.aria` 后缀剥离（spec 末段以 `.aria` 结尾时剥去，查找时统一补回，`lib/math` ≡ `lib/math.aria`）。
- 相对判定（`./` / `../` 开头，或正是 `.` / `..`）走 caller-local 单一基
  `dirname(current_module_path)`（**不碰 `source_roots`**，故相对导入永不逃逸到别的源根）；
  空基判空直接返 `nullopt`（拒绝锚定）。
- 裸名以 `source_roots` 为基，逐个 exists-check，首个 `<base>/<spec>.aria` 存在者命中。
- 逐基 `weakly_canonical(base / "<spec>.aria")` -> `exists` 则其 `string()` 为键；都不命中 -> `nullopt`；
  符号链接经 `weakly_canonical` 规避双加载。
- 越界 / 沙箱检测（相对导入越出源根）不在本原语职责内，留待加载层据源根列表判定。

> 完整规则表、示例与边界见 [`import-path-resolution.md`](./import-path-resolution.md)。

### IMPORT 操作码执行

`src/runtime/AriaVM.cpp` 主循环 `case OpCode::IMPORT` 收口于私有成员
`run_import(const ObjString* path)`
（bool 契约同 call_* 族：false ⟺ 载荷已 raise，unwind 留 dispatch_loop 调用点），步骤：

1. case 站点 `read_name(frame)` 读一个 u16 常量池索引取 `ObjString* path` 传入（良构前提是常量
   必为 intern 的 `ObjString*`）。
2. 以导入方帧模块 `abs_path()`（`run_import` 内经 `current_->frames().top()` 自取）为当前模块
   绝对路径，传 `resolve_module` 解析：
   - **失败**（`nullopt`，无源根命中）：经 `fail` 报 `ModuleNotFound`
     （`"module not found: '<path>'"`，带 IMPORT 站点位置）。
   - **成功**：`new_string(gc_, ...)` 把绝对键 intern 驻留为 `ObjString*`。
3. 以绝对键 `Value::from_obj(key)` 在 VM 模块表 `modules_`（`AriaHashTable`）查：
   - **命中**（编译成功才入表,表内进度 = 体待 run-once / 跑中 / 已跑完）：`module = module_entry->value`。
     命中正在 run-once 的模块即循环导入,按文法直接用其半初始化对象不报错。
   - **未命中**：调 `load_module(key, path)` 加载 + 编译（见下「加载层」），其返回值来源与错误
     契约见该节。
4. **压模块值于栈顶**（`current_->push(module)`，栈效应 `... -> [module]`）。命中分支 module 经
   `modules_` 根可达（非移动 GC，push 期间指针稳定，无需守卫）。
5. 绑定不在 IMPORT 内，由 CodeGen 按作用域走（见下）。

> **按作用域绑定**：`IMPORT path:u16` 仅取模块对象压栈，不带 `alias` 操作数。CodeGen
> `visitImportStmtNode` 按 `is_global_scope()` 分派（与 `var`/`fun` 同形 lowering）：顶层
> `declare_global` + `IMPORT` + `DEF_GLOBAL alias`；嵌套（函数体/块内）`declare_local` + `IMPORT`
> （值填槽）。对齐文法「绑模块到当前作用域（函数体=局部）」。

**根安全**：见下「加载层 load_module」节的根安全注。命中分支的 `module` 经 `modules_` 根可达，`current_->push` 期间指针稳定（非移动 GC），无需守卫；`dispatch_loop()` 不持 `LockGuard`。

### 源根列表（已实现，被 IMPORT 消费）

- `src/runtime/AriaVM.hpp`：`source_roots()` 访问器（返回有效列表）+ `set_source_roots(List<String>)`
  覆盖配置根。语义对齐 Python `sys.path`--解析器沿各源根找 `<源根>/<spec>.aria`，首个存在者命中；
  **命中文件的绝对规范路径为模块表键（源根不进键）**。
- `AriaVM.hpp`：成员 `AriaHashTable modules_;`、`List<String> source_roots_;`（单一列表，按槽位
  分区：`[0]` = 入口槽、`[1..]` = 配置根）。存为 `List<String>`（路径元数据），**非 `ObjString*`，
  不参与 GC 追踪**。
- `AriaVM.cpp` 构造：`source_roots_[0]` 占位为当前工作目录（`fs::current_dir()`；前期源根）；由 `fs::program_dir()`
  推导 stdlib 源根（约定 `<exe_dir>/../share/aria/lib`，经 `weakly_canonical` 规范化），非空则
  `push_back` 进 `[1..]`（stdlib 即一个配置源根）。
- `AriaVM.cpp` `run()`：`source_roots_[0] = 入口模块 dir_->view()`（原地替换构造时的 cwd 占位），
  `[1..]` 不动。`dir_` 指针恒非空（构造断言）、内容可空（cwd 不可用时空串兜底）--空串播种后
  裸名解析跳过空根。无 flag / 无重建--`[0]` 槽位
  约定 + 原地赋值，reuse 安全不累积旧入口根。`set_source_roots` 经 `resize(1)` 保留 `[0]`、替换
  `[1..]`（置空即清掉默认 stdlib）。**`source_roots_` 已被 IMPORT 的裸名解析消费**。
- 模块表 `modules_` 经 VM 根 tracer 注册进 GC（构造里 `gc_.set_vm_roots(...)`，tracer 标
  `modules_` + `builtins_` + `current_` 沿 `previous_` 执行链各上下文的值栈/帧/挂起错误寄存器）。
  键 = 绝对规范路径
  `ObjString*`（intern），值 = `ObjModule*`，均装箱为 `Value` 入 `AriaHashTable`。

### 加载层 load_module

IMPORT 未命中分支经 `load_module(canonical_path, import_specifier)`（`src/runtime/AriaVM.cpp`
私有成员）加载 + 编译：

1. `SourceFile::from_path(canonical_path)` 读文件（已处理 BOM/CRLF/UTF-8）。
2. `fs::module_name_and_dir(canonical_path)` 派生身份 `{name=stem, dir=dirname}`（同入口约定，
   `abs_path()` 还原 canonical key）-> `new_module` + `make_guard`（guard 跨编译与入表）。
3. `Compiler::compile(gc_, source, module, "<module>")` 编译（纯静态入口；`set_entry` 由 `CodeGen::init_module`
   编译期挂入）；编译成功才 `modules_.set` 入表（供循环导入命中体执行中的对象；加载事实源 =
   表成员资格，对象无状态字段；失败一律不留表项，同路径重试重新加载），返回模块（体待 run-once）。
4. IMPORT 未命中分支以其 `entry` 作**普通 0 参函数调用**进帧交主循环执行（run-once）；模块体入口
   的返回值恒为模块对象（编译器在入口收尾发射「压模块对象常量 + RETURN」），RETURN 通用写回
   callee 槽即完成栈效应；**无递归 `dispatch_loop()`**。

**错误契约同 call_closure 族**：返 `ObjModule*`，失败 `nullptr ⟺` 载荷已 raise 入 `*current_`
寄存器，调用方 `unwind()` 派发/物化；读盘失败/name 空经 `fail` 报 `ModuleNotFound`（带 IMPORT
站点位置，与 resolve_module 失败形态统一），编译期 Error 就地 `new_exception` 原样装配箱透传
（含被导入文件位置，不重烘）。

**根安全**：path 经常量池根；`canonical_path`(intern weak root) 经 `run_import` 的
`gc_.make_guard(canonical_path)` 跨 `load_module` 内一串 new_* 分配根化；`modules_.set` 等 rehash
走 trivial 分配不触 GC（GC 核心不变式），不是守卫承重点；`module`/`entry` 经 `modules_`+`module->entry_` 根可达。

**后续缺口**：目录包 `index.aria` 查找、相对导入越出源根的检测（`weakly_canonical` 折叠 `..`
后据源根列表判定报错）、解析缓存（IMPORT 重复执行同一 specifier 避免重复 stat，见
[`import-path-resolution.md`](./import-path-resolution.md)「当前边界与后续」）均留待后续。

## 测试覆盖

`tests/runtime/test_ariavm.cpp` 一组 IMPORT 测试，用 `testing::TempDir()` 建真实空 `.aria` 文件让
`resolve_module` 的 exists-check 命中，按解析出的绝对键预注册合成模块入 `vm.modules()`，触发
命中分支：

- `ImportBindsPreRegisteredModule`：入口根 `lib/utils.aria` 命中预注册模块（模拟循环导入半初始化）-> IMPORT 压栈
  -> `DEF_GLOBAL` 绑入 globals -> `LOAD_GLOBAL` 取回同一对象。
- `ImportNotFoundErrors`：无源根命中 `nope/missing.aria` -> `ModuleNotFound`。
- `ImportNormalizesAbsolutePath`：`lib/./utils` 经 `weakly_canonical` 折 `.` -> 命中
  `lib/utils.aria`。
- `ImportNormalizesRelativePath`：导入方 `dir_`=`base/lib`、`name_`=`main` + `./helper` ->
  相对基 `dirname(abs_path)` = `base/lib` = `dir_` -> `base/lib/helper.aria`。
- `ImportBareSearchesSourceRoots`：入口根无 `lib/math.aria` -> 裸名 fall-through 到 stdlib
  第二根命中（证明裸名用 `source_roots` 列表，非当前模块目录）。
- `ImportStripsAriaSuffix`：裸名 `lib/math.aria` -> 剥后缀再补回 -> 命中 `lib/math.aria`。
- `ImportStripsAriaSuffixOnRelative`：相对 `./math.aria` -> 剥后缀 -> 命中 `lib/math.aria`
  （与裸名版互补）。
- `SourceRootSeededFromEntryModuleDir` / `SourceRootSeededWithCwdForScriptEntry`：`run()`
  把入口模块 `dir_` 原地写入入口槽 `[0]`（磁盘入口 `dir_` = base -> `[base]`；合成
  `<script>` 经 `make_module` 注入 `dir_` = cwd -> `[cwd]`，stdlib 置空时列表大小为 1）。

其余覆盖：`tests/compile/test_parser.cpp`（`import "math" as m;` 解析）、`tests/compile/test_ast.cpp`
（`ImportStmtNode` dump）、`tests/compile/test_astvisitor.cpp`（visitor 桩）、`tests/compile/test_lexer.cpp`
（`import` / `as` 关键字）。入口返回值语义（返回值恒为模块对象）由
`tests/compile/test_codegen.cpp` 的 `EntryReturnYieldsModuleObject` / `ModuleEntryEpilogueLoadsModuleConstant`
/ `TopLevelBareReturnCompiles` / `ErrTopLevelReturnValue` 与语料
`positive/09_modules/module_early_return`（裸 `return;` 早退 + IMPORT 栈效应）、
`negative/compile_errors/compile_top_level_return_value`（顶层带值 return 拒绝）钉住。
`tests/runtime/test_ariavm.cpp` 新增加载层端到端测试（`ImportLoadsDiskModuleRunsBodyAndPopulatesGlobals`/`ImportModuleCompileErrorPropagates`/`ImportModuleRuntimeErrorPropagates`/`CircularImportCompletesBothLoaded`/`ReimportReusesLoadedModule`，stress GC 下经 `interpret_from_path` 跑真实 `.aria` 文件并白盒检视 `modules_`）。

## 相关文档

- [`import-path-resolution.md`](./import-path-resolution.md)：路径解析（裸名 / 相对 / 绝对规范键、
  逐源根 exists-check、`.aria` 后缀、模块表命中与未命中、根安全、示例、边界）的专文；
  ObjModule `dir_`/`name_`/`abs_path()` 字段规格亦在此文。
- [`bytecode-instruction-set.md`](../bytecode/bytecode-instruction-set.md) §4.15 / §5.9：IMPORT 指令
  格式与模块导入设计段。
- [`vm-design.md`](./vm-design.md)：`ObjModule` / 模块表 / `DEF/LOAD/STORE_GLOBAL` 的分阶段
  规划（M2 起用）。