# 导入路径解析

> 本文描述 `OpCode::IMPORT` 当前实现的路径解析与模块查表逻辑。指令格式与栈效应见
> `bytecode-instruction-set.md §4.15`，本文只聚焦「specifier -> 绝对规范键 -> 模块表命中」
> 这条链路及其当前边界（磁盘加载 / 编译 / run-once 尚未就绪）。

## 总览

`import "PATH" as ALIAS` 经三步落地（`src/runtime/AriaVM.cpp`）：

1. **解析为绝对规范键**：把 import 串 `PATH` 经 `resolve_module`（匿名命名空间）解析为
   **命中文件的绝对规范路径**（`std::filesystem::weakly_canonical`：解析符号链接、折叠
   `.`/`..`、去冗余分隔符）。相对 / 裸名是同一原语，仅基目录列表不同（见下）。
2. **intern 驻留**：经 `new_string(gc_, key_str)` 把绝对键驻留为 `ObjString*`。同内容共享
   同一指针，模块表用 `===` 严格相等查表，天然去重；符号链接经 `weakly_canonical` 规避双加载。
3. **模块表查表 + 压栈**：以绝对键 `ObjString*`（装箱为 `Value`）在 VM 模块表 `modules_`
   （`AriaHashTable`）里查；命中即复用模块对象并 `ctx.push` 压栈（`IMPORT path:u16`，栈效应
   `... -> [module]`）。绑定不在 IMPORT 内——交 CodeGen 按作用域经 `DEF_GLOBAL`（顶层）/ 值填槽
   + `mark_initialized`（嵌套）走。未命中走「未实现」分支（见下）。

模块表 `modules_`：键 = 绝对规范路径 `ObjString*`（intern），值 = `ObjModule*`，均装箱为
`Value` 入 `AriaHashTable`。`modules_` 经 VM 根 tracer 纳入 GC（`gc_.set_vm_roots`）。

## 核心：键 = 绝对规范路径

模块表键为命中文件的**绝对规范路径**：

- **跨根不碰撞**：`<proj>/util/tool.aria` 与 `<stdlib>/util/tool.aria` 是不同键，可共存。
- **相对导入不逃逸**：相对导入的基恒为当前模块自己的目录，永远落在自己所在树。
- **符号链接不双加载**：`weakly_canonical` 解析符号链接，等价路径归一到真实路径。
- **代价**：失去「按逻辑名跨位置导入」与字节码跨机器可移植性（键含机器路径）；aria
  编译 + 运行一体形态可接受。运行期 resolve，字节码不烘焙绝对路径。

## 统一解析原语

`resolve_module(specifier, current_abs, source_roots) -> Opt<String>`，相对与裸名是同一原语，
仅基目录列表不同：

- **相对导入**（以 `./` / `../` 开头，或正是 `.` / `..`）：基 = `dirname(current_abs)`
  （单一基，caller-local，**不碰 `source_roots`**），故相对导入永不逃逸到别的源根。
  其中 `current_abs` = `frame.module->abs_path()` = `root_ + "/" + name_ + ".aria"`（见
  `ObjModule::abs_path`，`.aria` 在末段，`dirname` 不受影响，等价 `dirname(root_ + "/" + name_)`）
  = `root_ + "/" + dirname(name_)` = 当前模块所在目录。`root_` 恒非空（`new_module` 默认 cwd），
  故 `current_abs` 恒非空 -- 合成模块（如 `<script>`，`root_` = cwd）相对导入以 cwd 为基。
- **裸名导入**（无 `./` `../` 前缀）：基 = `source_roots`（见下「裸名解析顺序」），逐个
  exists-check，首个存在 `<base>/<spec>.aria` 者命中。
- 返回命中文件的绝对规范路径 = 模块表键；都不存在即 `nullopt`（-> `ModuleNotFound`）。

## 裸名解析顺序

`source_roots`（`AriaVM::source_roots_`，`List<String>`，按顺序）：

1. **入口槽 `source_roots_[0]`**：入口模块的 `root_`（其所属源根目录，对齐 Python
   `sys.path[0]`；整次运行固定，不随导入方文件变化）。VM 构造时先以**当前工作目录**占住
   `[0]`（前期源根），`run()` 时用入口模块 `root_` 原地替换（前置：入口模块 `root_` 非空）。
2. **编译器相对 stdlib 目录**（约定，VM 构造时由 `fs::program_dir()` 推导，如
   `<exe_dir>/../share/aria/lib`，经 `weakly_canonical` 规范化后推入 `source_roots_[1..]`；
   确切路径待定）。
3. **其余源根**（`-L` 标志、`ARIA_PATH` 环境变量等）—— 之后再加（同样入 `[1..]`）。

存储为单一 `source_roots_`（`List<String>`），按槽位约定分区，无 flag / 无并列配置列表：
`[0]` = 入口槽（构造占 cwd，`run()` 换成入口 `root_`），`[1..]` = 配置根（stdlib / `-L` /
环境变量）。`set_source_roots(List<String>)` 替换 `[1..]`、保留 `[0]`（`resize(1)` 后追加），
故 reuse 安全不累积旧入口根。`set_source_roots` 置空即清掉默认 stdlib（隔离）；置
`[dir...]` 即指定自定义源根集合。

> **裸名不试当前模块所在目录**（Python 3 `sys.path[0]` 风格，非 Python 2 隐式相对）：
> 裸名 `import "math"` 在任何文件里都一致地指「入口根级或 stdlib 的 math」，不随所在文件
> 变化；导入同目录兄弟须用 `import "./math"` 显式相对。这样把「同名劫持 stdlib」的潜在点
> 从「任意子目录」收窄到「只有入口根目录」，且裸名行为可预测。
>
> 残余副作用：入口根目录下放一个 `math.aria` 仍会盖掉 stdlib `math`——这是「入口目录在
> `source_roots` 里」的固有后果，非此选择引入。

> 注：`source_roots_` 存为 `List<String>`（路径元数据），非 `ObjString*`，不参与 GC 追踪——
> 它不作模块表键，仅解析器用。

## 相对路径 = 相对当前模块目录

`./`、`../`、`.`、`..` 开头的路径相对**当前模块所在目录**解析：基 = `dirname(current_abs)`
（`current_abs` = `frame.module->abs_path()` = `root_ + "/" + name_ + ".aria"`，见
`ObjModule::abs_path`）。`.aria` 后缀在末段，`dirname` 不受影响（等价于 `dirname(root_ + "/" +
name_)`）。`root_` 恒非空（`new_module` 默认 cwd），故 `current_abs` 恒非空 -- 合成模块（如
测试桩 `<script>`，`root_` = cwd）相对导入以 cwd 为基。

例：当前模块 `root_` = `/proj`、`name_` = `lib/main`（abs_path = `/proj/lib/main.aria`），
`import "./helper"` -> 基 `dirname(/proj/lib/main.aria)` = `/proj/lib` -> `/proj/lib/helper.aria`。

## 文件查找约定

- `<spec>` -> `<base>/<spec>.aria`（逐基 exists-check，首个存在者命中）。
- **末尾 `.aria` 可选**：spec 末段长度 > 5 且以 `.aria` 结尾时剥离（`lib/math.aria` ->
  `lib/math`），查找时统一补回 `.aria`，使 `lib/math` ≡ `lib/math.aria`。
- **目录包**（`<base>/<spec>/index.aria`）属后期特性，当前不支持：找不到
  `<base>/<spec>.aria` 即跳过该基 / `ModuleNotFound`。

## ObjModule 的 root_ / name_ 字段

`ObjModule` 持 `root_` + `name_`（均 `ObjString*`，intern），模块的绝对路径由二者合成
（不再单独存 `abs_path_` 字段，由 `abs_path()` 方法即时合成）：

- `root_` = 模块所属的**源根目录**（如 `/proj`）；`name_` = 相对源根的路径（如 `lib/utils`）。
- 模块绝对路径（= 模块表查重键）= `root_ + "/" + name_ + ".aria"`，由 `abs_path()` 返回；
  相对导入基目录 = `dirname(abs_path())` = `root_ + "/" + dirname(name_)`；`run()` 把入口模块
  `root_` 播种为 `source_roots_[0]`。
- 加载层落地后由 loader 在 `resolve_module` 命中时定 `root_`（命中哪个源根即哪个）、`name_`
  （specifier 相对该源根的路径）；**`root_` 恒非空** -- `new_module` 未显式传 `root` 时取当前
  工作目录作默认（合成模块如测试桩 `<script>` 退化为 cwd，仍能合成绝对路径、作相对导入基、
  进源根播种）。
- `name_` 兼作显示名（`to_string` / 报错渲染），不单独参与模块表查重。

`run()` 时：`source_roots_[0]` = 入口模块 `root_`（原地替换构造时的 cwd 占位），`[1..]`
不动。`root_` 恒非空（`new_module` 默认 cwd），故无需空检查；合成入口模块（如测试桩
`<script>`）`root_` = cwd，入口槽即 cwd 退化值。

## 模块表命中与未命中

IMPORT 以绝对键查 `modules_`：

- **命中**（模块对象任意态）：复用该模块对象。
  - `Loaded` = 完整模块。
  - `Loading` = 循环导入命中的「半初始化对象」——按文法「允许循环导入，命中正在初始化的
    模块返回半初始化对象」直接用，不报错。
- **未命中**（文件解析命中但模块未入表）：调 `load_module(key, path)` 得模块(Loading，已编译 `set_entry`)，
  IMPORT 未命中分支以其 `entry` 作**普通 0 参函数调用**进帧(`call_value`)后 break。模块体 run-once 即执行
  一个函数,由主循环照常驱动;其 RETURN 按函数名 == `<module>` 判定模块体帧,弹弃返回值、置该模块 `Loaded`、
  改压模块对象(模块体「返回模块」),故命中/未命中栈效应统一 `[..., module]`。**无递归 `run_()`**。被导入模块的
  编译期/运行期错误原样透传(含其文件位置);读盘失败/name 空报 `ErrorCode::ModuleNotFound`。详见
  `import-handling-overview.md`「加载层接入位置」。
- **解析失败**（无源根命中 `<base>/<spec>.aria`）：返回 `ErrorCode::ModuleNotFound`
  （`module not found: 'PATH' (no matching source root)`）。

命中后，`ctx.push(module)` 把模块对象压栈（`IMPORT path:u16`，栈效应 `... -> [module]`）；
绑定交 CodeGen 按作用域走（顶层 `DEF_GLOBAL` / 嵌套值填槽 + `mark_initialized`）。

## 根安全

GC 已启用（VM 根 tracer 标 `modules_` + 值栈 + 帧），`run_()` 不持 `LockGuard`：

- `path` 经常量池根（同 `LOAD_CONST`）。
- 绝对键经 `new_string` intern 驻留（weak root）。
- 命中分支取回的 `module` 经 `modules_` 根可达，`ctx.push` 期间指针稳定（非移动 GC），无需守卫。

## 示例

设入口模块 `root_` = `/proj`、`name_` = `main`（abs_path = `/proj/main.aria`，入口源根 =
`/proj`），stdlib = `/stdlib`，当前模块 `root_` = `/proj`、`name_` = `a/b`
（abs_path = `/proj/a/b.aria`）：

| import 串 | 类别 | 基 | 绝对键 |
| :--- | :--- | :--- | :--- |
| `"lib/utils"` | 裸名 | source_roots | `/proj/lib/utils.aria`（入口根命中） |
| `"lib/utils.aria"` | 裸名，剥 `.aria` | source_roots | `/proj/lib/utils.aria` |
| `"./helper"` | 相对 | `dirname(cur)` = `/proj/a` | `/proj/a/helper.aria` |
| `"../x"` | 相对 | `/proj/a`，`..` 折叠 | `/proj/x.aria` |
| `"lib/./utils"` | 裸名，`weakly_canonical` 折 `.` | source_roots | `/proj/lib/utils.aria` |
| `"math"`（入口根无，stdlib 有） | 裸名，fall-through | source_roots | `/stdlib/math.aria` |

注意裸名不取当前模块目录：`/proj/a/b.aria` 里 `import "lib/math"` 解析到
`/proj/lib/math.aria`（相对入口源根），**不是** `/proj/a/lib/math.aria`。导入同目录兄弟
须用 `import "./math"` 显式相对。

## 当前边界与后续

- **已落地**：绝对键解析（`resolve_module` + `weakly_canonical` + 逐基 exists-check）、
  `source_roots` 播种（`[0]` 入口槽 cwd 占位 + `run()` 换入口 `root_`、`[1..]` 配置根 stdlib）、
  `ObjModule::root_`/`name_` + `abs_path()`（合成绝对路径，`root_` 恒非空 -- `new_module` 默认
  cwd）、`.aria` 后缀剥离、模块表命中复用（含 `Loading` 半初始化）、循环导入语义、**未命中分支
  加载链路**（`load_module`：读文件 → `Compiler::compile` 编为被导入模块 CodeUnit（入口名 `<module>`、
  `set_entry`）→ 入表 Loading；IMPORT 未命中分支以 `entry` 作普通 0 参函数调用进帧交主循环 run-once,其
  RETURN 按函数名 == `<module>` 判定模块体帧后置 `Loaded` + 压回模块对象,无递归 `run_()`）。
- **解析缓存**：IMPORT 重复执行同一 specifier 需避免重复 stat。计划加一层缓存，键
  `(当前模块绝对目录, specifier ObjString*)` -> 已解析绝对键 `ObjString*`，命中即跳过磁盘。
  确切结构 / 存放位置待定（TODO）。
- **stdlib 路径**：`<exe_dir>/../share/aria/lib` 为约定初值，确切路径与平台分流待定。
- **其余源根**：`-L` 标志、`ARIA_PATH` 环境变量等之后再加。
- **源根越界**：相对导入越出源根的检测留待加载层（`weakly_canonical` 折叠 `..` 后由加载层
  据源根列表判定，本原语不限路径范围）。

> 全景与各环节状态（词法 / 语法 / 编译器缺口 / 字节码 / VM / ObjModule / fs 原语 / 测试）
> 另见 [`import-handling-overview.md`](./import-handling-overview.md)。