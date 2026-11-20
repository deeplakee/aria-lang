# 导入（import）

`import "PATH" as ALIAS;` 的指令格式与栈效应见 [指令集规格](instruction-set.md) §3.14。
`ObjModule` 字段规格见 [模块身份](../object.md#模块身份)。

## 文法与前端

- 文法（`docs/grammar.txt` 的 `importStmt`）：`"import" string "as" identifier ";"`，导入名用字符串，`as` 别名必写。
- 关键字：`TokenType::Import` / `TokenType::As`。
- Parser 的 `import_stmt()` 只做形状检查（path 位须为 `String` token，否则 `ExpectedToken`：`expected a string literal as module path, got '...'`），`string_value()` + `shape()` 组 `StringLiteralNode`；**不做任何路径解析或文件系统检查**。
- `ImportStmtNode{ path: StringLiteralNode*, alias: StringView }`，`dump` 渲染 `ImportStmt as=...`。
- CodeGen 的 `visitImportStmtNode` 把 path 经 `add_string_literal_or_fail`（与字面量表达式共用驻留口）得常量索引，发射 `IMPORT path:u16`；**绑定在 IMPORT 之后**，按作用域走（顶层声明并 `DEF_GLOBAL alias` / 嵌套值填槽），与 `var`/`fun` 同形。
- `OpCode::IMPORT` 格式类别 `Import`，栈效应 `... -> [module]`；反汇编渲染 `IMPORT PPPP  ; path`。

## 模块表键

模块表的键为驻留字符串，用 `===` 查表。文件模块用命中文件的**绝对规范路径**（`weakly_canonical`：解析符号链接、折叠 `.`/`..`、去冗余分隔符）；原生标准库用**完整逻辑名**（如 `std:math`）。

- 跨源根不碰撞：`<proj>/util/tool.aria` 与 `<stdlib>/util/tool.aria` 是不同键。
- 符号链接不双加载：等价路径归一到真实路径。
- 导入在运行期解析，字节码保留导入字符串。

## 原生标准库

`std:` 前缀在路径解析之前识别。标准库名字区分大小写，完整字符串与注册表精确匹配。
`std:Math`、`std:math.aria`、`std:` 等未注册名都以原字符串报 `ModuleNotFound`。
项目中的 `math.aria` 经普通文件导入规则查找。

注册表提供每个原生模块的成员注册函数。原生模块使用完整名和空目录，`entry()` 为空。
普通原生函数可用 `Builtin::register_functions` 注册进已根化模块的 `globals()`。

## 文件路径解析

`AriaVM::resolve_module(specifier, current_module_path)` 查找文件，返回规范路径或空值。
文件模块的相对和非相对路径使用不同的基目录：

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

裸名导入在各模块中都走同一组源根。同目录兄弟用 `import "./math" as math;`。

### 示例

入口模块 `dir_` = `/proj`（入口源根 `/proj`），stdlib = `/stdlib`，导入方 `dir_` = `/proj/a`、`name_` = `b`（`abs_path` = `/proj/a/b.aria`）：

| import 串 | 类别 | 基 | 模块表键 |
| :--- | :--- | :--- | :--- |
| `"lib/utils"` | 裸名 | source_roots | `/proj/lib/utils.aria` |
| `"lib/utils.aria"` | 裸名，剥后缀 | source_roots | `/proj/lib/utils.aria` |
| `"./helper"` | 相对 | `dirname(cur)` = `/proj/a` | `/proj/a/helper.aria` |
| `"../x"` | 相对，`..` 折叠 | `/proj/a` | `/proj/x.aria` |
| `"math"`（入口根无、配置根有） | 裸名 | source_roots | `/stdlib/math.aria` |
| `"std:math"` | 原生标准库 | 标准库注册表 | `std:math` |

`/proj/a/b.aria` 里 `import "lib/math"` 解析到 `/proj/lib/math.aria`，**不是** `/proj/a/lib/math.aria`。

## `ObjModule` 的身份字段

`dir_` 与 `name_` 均为驻留字符串。文件模块经 `fs::module_name_and_dir` 切分，`dir_` 是
dirname，`name_` 是 stem。原生标准库模块使用空目录和完整逻辑名。

- `dir_` 指针恒非空、内容可空（`new_module` 未传时取当前工作目录，不可用则空串）。
- `name_` 兼作显示名（`repr` 与报错渲染）。`abs_path()` 从目录和模块名即时合成，空目录返回空串。
- 模块表已有条目时复用该模块。文件模块在编译后入表，此时可能正在执行模块体；原生模块入表时成员已经就位。

## `IMPORT` 执行体（`Eval::run_import`）

`run_import` 按 `std:` 前缀分派给 `import_stdlib_module` 或 `import_file_module`。
两条路径查到缓存时直接压模块对象。失败返回 false，由 dispatch_loop 处理已挂起的错误。

- `import_stdlib_module` 用常量池里的完整逻辑名作键。缓存未命中时，`load_stdlib_module` 构造原生模块，
  注册成员后入表，再由 `import_stdlib_module` 压栈。
- `import_file_module` 调用 `resolve_module` 查找文件，驻留规范路径作键。缓存未命中时调用
  `load_file_module` 读文件、编译并入表。返回后检查模块入口，再从入口原型创建函数，经
  `call_aria_fn` 进帧。主循环驱动模块体，RETURN 将模块对象写回 callee 槽。循环导入可以复用
  表中半初始化的模块。求值失败时保留该模块。

读盘失败或文件名无效时挂起 `ModuleNotFound`。编译期 `Error` 原样装箱，保留源码位置。

原始导入名由当前帧的常量池保活。`import_file_module` 在加载期间根化新驻留的路径，VM 在注册或编译期间根化模块。
两个加载方法在返回前入表，随后由模块表保活模块和入口原型。

## 不支持

- 目录包（`<base>/<spec>/index.aria`）：只认 `<base>/<spec>.aria`。
- 相对导入越出源根的 containment 检查：解析器不限路径范围。
- 文件路径解析缓存：每次导入文件模块都重新查找并规范化路径。
