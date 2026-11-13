# 导入（import）

`import "PATH" as ALIAS;` 的端到端形态：文法 -> 字节码 -> VM 路径解析 -> 模块表 -> 加载与 run-once。指令格式与栈效应见 `instruction-set.md` §3.14；`ObjModule` 字段规格见 object.md「模块身份」。

## 文法与前端

- 文法（`docs/grammar.txt` 的 `importStmt`）：`"import" string "as" identifier ";"`--强制字符串路径、强制 `as` 别名，不支持裸名导入。
- 关键字：`TokenType::Import` / `TokenType::As`。
- Parser 的 `import_stmt()` 只做形状检查（path 位须为 `String` token，否则 `ExpectedToken`：`expected a string literal as module path, got '...'`），`string_value()` + `shape()` 组 `StringLiteralNode`；**不做任何路径解析或文件系统检查**。
- `ImportStmtNode{ path: StringLiteralNode*, alias: StringView }`，`dump` 渲染 `ImportStmt as=...`。
- CodeGen 的 `visitImportStmtNode` 把 path 经 `add_string_literal_or_fail`（与字面量表达式共用驻留口）得常量索引，发射 `IMPORT path:u16`；**绑定在 IMPORT 之后**，按作用域走（顶层声明并 `DEF_GLOBAL alias` / 嵌套值填槽），与 `var`/`fun` 同形。
- `OpCode::IMPORT` 格式类别 `Import`，栈效应 `... -> [module]`；反汇编渲染 `IMPORT PPPP  ; path`。

## 键 = 绝对规范路径

模块表键为命中文件的**绝对规范路径**（`weakly_canonical`：解析符号链接、折叠 `.`/`..`、去冗余分隔符），经 `new_string` 驻留后用 `===` 查表。

- 跨源根不碰撞：`<proj>/util/tool.aria` 与 `<stdlib>/util/tool.aria` 是不同键。
- 符号链接不双加载：等价路径归一到真实路径。
- 代价：失去「按逻辑名跨位置导入」与字节码跨机器可移植性（键含机器路径）。运行期解析，字节码不烘焙绝对路径。

## `resolve_module(spec, current_module_path) -> Opt<String>`

`AriaVM` 私有成员，源根取自身 `source_roots_`。相对与裸名共用本原语，只有基目录列表不同：

- **相对**（`./` / `../` 开头，或正是 `.` / `..`，Windows 风格 `.\` 不进此分支）：基 = `dirname(current_module_path)`，单一基、caller-local，**不碰 `source_roots`**--所以相对导入不会落到别的源根去搜索。基为空（`abs_path()` 为空）直接返 `nullopt`。
- **裸名**：基 = `source_roots_`，逐基 exists-check，首个命中的 `<base>/<spec>.aria` 即结果。
- `.aria` 后缀：spec 末段长度 > 5 且以 `.aria` 结尾时剥掉，查找时统一补回（`lib/math` ≡ `lib/math.aria`）。
- 任何基都不命中返 `nullopt` -> `ModuleNotFound`。**不做 containment 检查**：`../` 折出源根仍可解析成功。

注意 `current_module_path` 取的是导入方模块的 `abs_path()`（空判据也用它），不是 `parent_path()`。

### 裸名解析顺序

`source_roots_` 是单一 `List<String>`，按槽位分区：

1. `[0]` 入口槽：入口模块的 `dir_`（对齐 Python `sys.path[0]`，整次运行固定，不随导入方变化）。VM 构造时先占当前工作目录，`run()` 原地换成入口 `dir_`；内容为空时该根被跳过。
2. `[1..]` 配置根：stdlib（`kStdlibRelPath = "../share/aria/lib"`，构造时由 `fs::program_dir()` 推导、`weakly_canonical` 规范化后推入）。
3. `set_source_roots(List<String>)` 整体替换 `[1..]`、保留 `[0]`（`resize(1)` 后追加），故复用 VM 不累积旧入口根；置空即清掉默认 stdlib。当前消费者仅测试--CLI 无配置通道，也没有 `-L`/`ARIA_PATH`。

存储为 `List<String>`（路径元数据），**不是 `ObjString*`、不参与 GC 追踪**（它不作表键，只有解析器读）。

**裸名不试当前模块所在目录**（Python 3 `sys.path[0]` 风格，非 Python 2 隐式相对）：裸名在任何文件里都一致指「入口根级或 stdlib 的同名模块」，导入同目录兄弟须写 `import "./math"`。把「同名劫持 stdlib」的潜在点从任意子目录收窄到入口根目录；残余副作用是入口根下的同名文件仍会盖掉 stdlib 版本。

### 示例

入口模块 `dir_` = `/proj`（入口源根 `/proj`），stdlib = `/stdlib`，导入方 `dir_` = `/proj/a`、`name_` = `b`（`abs_path` = `/proj/a/b.aria`）：

| import 串 | 类别 | 基 | 绝对键 |
| :--- | :--- | :--- | :--- |
| `"lib/utils"` | 裸名 | source_roots | `/proj/lib/utils.aria` |
| `"lib/utils.aria"` | 裸名，剥后缀 | source_roots | `/proj/lib/utils.aria` |
| `"./helper"` | 相对 | `dirname(cur)` = `/proj/a` | `/proj/a/helper.aria` |
| `"../x"` | 相对，`..` 折叠 | `/proj/a` | `/proj/x.aria` |
| `"math"`（入口根无、stdlib 有） | 裸名 fall-through | source_roots | `/stdlib/math.aria` |

`/proj/a/b.aria` 里 `import "lib/math"` 解析到 `/proj/lib/math.aria`，**不是** `/proj/a/lib/math.aria`。

## `ObjModule` 的身份字段

`dir_` + `name_`（均 `ObjString*`、intern）由 `fs::module_name_and_dir` 按命中文件切分：`dir_` = dirname，`name_` = `stem()`。`abs_path()` 即时合成 `dir_ + "/" + name_ + ".aria"`，不存 `abs_path_` 字段。

- `dir_` 指针恒非空、内容可空（`new_module` 未传时取当前工作目录，不可用则空串）。
- `name_` 兼作显示名（`repr` 与报错渲染），不单独参与查重。
- **加载层与入口层产出的模块** `name_` 是单段 stem（不含 `/`），故 `dirname(abs_path()) == dir_`；`ObjModule` 构造只断言非空，测试会直接造多段 `name_` 的模块，别把该等式当类型不变式。
- 无加载状态字段：加载事实源 = 模块表成员资格（未入表 = 未加载；入表 = 已编译待 run-once 或已跑完）。

## `IMPORT` 执行体（`Eval::run_import`）

bool 契约同 `call_*` 族（false ⟺ 载荷已 raise），unwind 留 dispatch_loop 调用点：

1. `read_name(frame)` 读 u16 常量池索引取 `ObjString* path`（良构前提：常量必为 intern 串）。
2. 取导入方帧模块的 `abs_path()` 作 `current_module_path` 调 `resolve_module`；失败报 `ModuleNotFound`（`module not found: '<path>'`）。
3. 成功则把绝对键 intern，以 `Value` 查 `modules_`：
   - **命中**：复用该模块对象（表内任意进度，见下）。
   - **未命中**：调 `load_module(key, path)` 得模块，由其入口原型现场建函数、`call_aria_fn` 进帧后 break--**模块体 run-once 就是一个普通 0 参函数**，由主循环照常驱动，无递归 `dispatch_loop()`。
4. 压模块值：命中臂 `current_->push(module)`；未命中臂压的是入口函数，模块值由模块体 RETURN 写回 callee 槽。模块体入口的返回值恒为模块对象（编译器在入口收尾发射「压模块对象常量 + RETURN」），故两臂栈效应统一 `[..., module]`。

**循环导入**：入表先于模块体执行，故正在 run-once 的模块已在表内，命中即拿到半初始化对象，按语言语义直接用、不报错。

## `load_module(canonical_path, import_specifier)`

`AriaVM` 私有，**仅加载与编译**，且**仅限 dispatch_loop 驱动期调用**（错误载荷随 `*current_` 走，run 外直调会被吞）：

1. `SourceFile::from_path(canonical_path)` + `fs::module_name_and_dir` -> `new_module` + `make_guard`（guard 跨编译与入表）。
2. `Compiler::compile(gc_, source, module, kModuleEntryName)`。
3. **编译成功才 `modules_.set`**（键 = canonical_path，一身二任：表键 + 读盘路径）。
4. 模块体执行在 IMPORT 分支（见上）。

**错误与表项**：返 `ObjModule*`，`nullptr` ⟺ 载荷已 raise。读盘失败 / `name` 空经 `fail` 报 `ModuleNotFound`；编译期 `Error` 就地装箱透传（消息自带位置，不重烘）。**只有编译失败不留表项**：入表先于模块体执行，模块体抛错或运行期失败后表项不回滚，后续 import 命中的就是那个半初始化模块。

**根安全**：`canonical_path` 是 intern weak root，跨 `load_module` 内一串 `new_*` 分配由调用方的 `make_guard` 根化；`modules_.set` 等表 rehash 走 trivial 分配、不触 GC，不是守卫承重点；`module`/`entry` 经 `modules_` 与 `module->entry_` 可达。命中臂取回的 `module` 同理（非移动 GC，push 期间指针稳定，不加守卫）。

## 不支持

- 目录包（`<base>/<spec>/index.aria`）：只认 `<base>/<spec>.aria`。
- 相对导入越出源根的 containment 检查：解析器不限路径范围。
- 解析缓存：IMPORT 每次执行都重新 stat/`weakly_canonical`。
