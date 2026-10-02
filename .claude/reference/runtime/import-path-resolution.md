# 导入路径解析

> 本文描述 `OpCode::IMPORT` 当前实现的路径解析与模块查表逻辑。指令格式与栈效应见
> `bytecode-instruction-set.md §4.15`，本文只聚焦「specifier -> 绝对规范键 -> 模块表命中」
> 这条链路。

## 总览

`import "PATH" as ALIAS` 经三步落地（`src/runtime/AriaVM.cpp`）：

1. **解析为绝对规范键**：把 import 串 `PATH` 经 `resolve_module`（匿名命名空间）解析为
   **命中文件的绝对规范路径**（`std::filesystem::weakly_canonical`：解析符号链接、折叠
   `.`/`..`、去冗余分隔符）。相对 / 裸名是同一原语，仅基目录列表不同（见下）。
2. **intern 驻留**：经 `new_string(gc_, key_str)` 把绝对键驻留为 `ObjString*`。同内容共享
   同一指针，模块表用 `===` 严格相等查表，天然去重；符号链接经 `weakly_canonical` 规避双加载。
3. **模块表查表 + 压栈**：以绝对键 `ObjString*`（装箱为 `Value`）在 VM 模块表 `modules_`
   （`AriaHashTable`）里查；命中即复用模块对象并 `current_->push` 压栈（`IMPORT path:u16`，栈效应
   `... -> [module]`）。绑定不在 IMPORT 内--交 CodeGen 按作用域经 `DEF_GLOBAL`（顶层）/ 值填槽
   （嵌套）走。未命中走 `load_module` 加载链路（见下）。

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

`resolve_module(spec, current_module_path, source_roots) -> Opt<String>`，相对与裸名是同一原语，
仅基目录列表不同：

- **相对导入**（以 `./` / `../` 开头，或正是 `.` / `..`）：基 = `dirname(current_module_path)`
  （单一基，caller-local，**不碰 `source_roots`**），故相对导入永不逃逸到别的源根。
  其中 `current_module_path` = `frame.module->abs_path()` = `dir_ + "/" + name_ + ".aria"`（见
  `ObjModule::abs_path`，`dir_` = 模块文件所在目录、`name_` = 文件名去 `.aria` 后缀/stem）。
  `.aria` 后缀在末段，`dirname` 不受影响 -> `dirname(dir_ + "/" + name_ + ".aria")` = `dir_`
  （name_ 为单段 stem，无 `/`）= 当前模块所在目录。`dir_` 指针恒非空但内容可空（`new_module`
  默认 cwd，cwd 不可用空串兜底），`abs_path()` 随之可返空串；相对分支对空基判空直接返
  `nullopt`（拒绝锚定），故相对解析可失败。
- **裸名导入**（无 `./` `../` 前缀）：基 = `source_roots`（见下「裸名解析顺序」），逐个
  exists-check，首个存在 `<base>/<spec>.aria` 者命中。
- 返回命中文件的绝对规范路径 = 模块表键；都不存在即 `nullopt`（-> `ModuleNotFound`）。

## 裸名解析顺序

`source_roots`（`AriaVM::source_roots_`，`List<String>`，按顺序）：

1. **入口槽 `source_roots_[0]`**：入口模块的 `dir_`（即入口文件所在目录，对齐 Python
   `sys.path[0]`；整次运行固定，不随导入方文件变化）。VM 构造时先以**当前工作目录**占住
   `[0]`（前期源根），`run()` 时用入口模块 `dir_` 原地替换（前置：入口模块 `dir_` 非空）。
2. **编译器相对 stdlib 目录**（约定，VM 构造时由 `fs::program_dir()` 推导，如
   `<exe_dir>/../share/aria/lib`，经 `weakly_canonical` 规范化后推入 `source_roots_[1..]`；
   确切路径待定）。
3. **其余源根**（`-L` 标志、`ARIA_PATH` 环境变量等）-- 之后再加（同样入 `[1..]`）。

存储为单一 `source_roots_`（`List<String>`），按槽位约定分区，无 flag / 无并列配置列表：
`[0]` = 入口槽（构造占 cwd，`run()` 换成入口 `dir_`），`[1..]` = 配置根（stdlib / `-L` /
环境变量）。`set_source_roots(List<String>)` 替换 `[1..]`、保留 `[0]`（`resize(1)` 后追加），
故 reuse 安全不累积旧入口根。`set_source_roots` 置空即清掉默认 stdlib（隔离）；置
`[dir...]` 即指定自定义源根集合。

> **裸名不试当前模块所在目录**（Python 3 `sys.path[0]` 风格，非 Python 2 隐式相对）：
> 裸名 `import "math"` 在任何文件里都一致地指「入口根级或 stdlib 的 math」，不随所在文件
> 变化；导入同目录兄弟须用 `import "./math"` 显式相对。这样把「同名劫持 stdlib」的潜在点
> 从「任意子目录」收窄到「只有入口根目录」，且裸名行为可预测。
>
> 残余副作用：入口根目录下放一个 `math.aria` 仍会盖掉 stdlib `math`--这是「入口目录在
> `source_roots` 里」的固有后果，非此选择引入。

> 注：`source_roots_` 存为 `List<String>`（路径元数据），非 `ObjString*`，不参与 GC 追踪--
> 它不作模块表键，仅解析器用。

## 相对路径 = 相对当前模块目录

即「统一解析原语」的相对分支（细则见上），`.`/`..`/`./`/`../` 开头一律相对**当前模块所在目录**。

例：当前模块 `dir_` = `/proj/lib`、`name_` = `main`（abs_path = `/proj/lib/main.aria`），
`import "./helper"` -> 基 `dirname(/proj/lib/main.aria)` = `/proj/lib` = `dir_` -> `/proj/lib/helper.aria`。

## 文件查找约定

- `<spec>` -> `<base>/<spec>.aria`（逐基 exists-check，首个存在者命中）。
- **末尾 `.aria` 可选**：spec 末段长度 > 5 且以 `.aria` 结尾时剥离（`lib/math.aria` ->
  `lib/math`），查找时统一补回 `.aria`，使 `lib/math` ≡ `lib/math.aria`。
- **目录包**（`<base>/<spec>/index.aria`）属后期特性，当前不支持：找不到
  `<base>/<spec>.aria` 即跳过该基 / `ModuleNotFound`。

## ObjModule 的 dir_ / name_ 字段

`ObjModule` 持 `dir_` + `name_`（均 `ObjString*`，intern），模块的绝对路径由二者合成
（不再单独存 `abs_path_` 字段，由 `abs_path()` 方法即时合成）。二者即模块文件绝对路径的
dirname / stem 切分，由 `fs::module_name_and_dir` 按命中文件的绝对规范路径做（不依赖源根概念）：

- `dir_` = 模块文件所在目录（如 `/proj/lib`）；`name_` = 文件名去 `.aria` 后缀/stem（如 `utils`）。
  **`name_` 恒为单段 stem**（`module_name_and_dir` 取 `filename().stem()`，不含 `/`）；`dir_` 即
  模块文件所在目录，不支持「相对源根的多段路径」语义。
- 模块绝对路径（= 模块表查重键）= `dir_ + "/" + name_ + ".aria"`，由 `abs_path()` 返回；
  相对导入基目录 = `dirname(abs_path())` = `dir_`（name_ 单段 stem，`dirname(dir/name.aria)` = `dir`）；
  `run()` 把入口模块 `dir_` 播种为 `source_roots_[0]`（即入口文件所在目录，对齐 Python `sys.path[0]`）。
- 加载层在 `resolve_module` 命中后由 `fs::module_name_and_dir` 按命中文件绝对规范路径定 `dir_`（dirname）
  与 `name_`（stem）；**`dir_` 指针恒非空、内容可空** -- `new_module` 未显式传 `dir` 时取当前工作目录作默认
  （cwd 不可用空串兜底；内容空时 `abs_path()` 返空串，相对导入基/源根播种随之空转）。
- `name_` 兼作显示名（`to_string` / 报错渲染），不单独参与模块表查重。

`run()` 时：`source_roots_[0]` = 入口模块 `dir_`（原地替换构造时的 cwd 占位），`[1..]`
不动。`dir_` 指针恒非空、内容可空（cwd 不可用时空串兜底）--空串播种后裸名解析跳过空根。

## 模块表命中与未命中

IMPORT 以绝对键查 `modules_`：

- **命中**（编译成功才入表，故表内任意执行进度：体待 run-once / 正在 run-once / 已跑完；加载事实源 = 表成员资格、对象无状态字段）：复用该模块对象。
  - 体已跑完 = 完整模块。
  - 正在 run-once = 循环导入命中的「半初始化对象」--按文法「允许循环导入，命中正在初始化的
    模块返回半初始化对象」直接用，不报错。
- **未命中**（文件解析命中但模块未入表）：调 `load_module(key, path)` 得模块(已编译 `set_entry`，体待 run-once)，
  IMPORT 未命中分支以其 `entry` 现场包空闭包经 `call_closure` 进帧后 break。模块体 run-once 即执行
  一个函数,由主循环照常驱动;模块体入口的返回值恒为模块对象(编译器在入口收尾发射「压模块对象
  常量 + RETURN」,见 compile.md `FnKind::ModuleEntry`),RETURN 通用写回 callee 槽,故命中/未命中栈效应
  统一 `[..., module]`。**无递归 `dispatch_loop()`**。被导入模块的
  编译期/运行期错误原样透传(含其文件位置;`load_module` 错误契约同 call_closure 族:返 `ObjModule*`,失败
  `nullptr ⟺` 载荷已 raise 入寄存器,编译期 Error 就地 `new_exception` 原样装配箱,调用方 `unwind()` 派发/物化);
  读盘失败/name 空报 `ErrorCode::ModuleNotFound`(经 `fail` 烘 IMPORT 站点位置)。详见
  `import-handling-overview.md`「加载层接入位置」。
- **解析失败**（无源根命中 `<base>/<spec>.aria`）：报 `ErrorCode::ModuleNotFound`
  （`module not found: '<path>'`，经 `fail` 烘 IMPORT 站点位置）。

命中后，`current_->push(module)` 把模块对象压栈（`IMPORT path:u16`，栈效应 `... -> [module]`）；
绑定交 CodeGen 按作用域走（顶层 `DEF_GLOBAL` / 嵌套值填槽）。

## 根安全

GC 已启用（VM 根 tracer 标 `modules_` + 值栈 + 帧），`dispatch_loop()` 不持 `LockGuard`：

- `path` 经常量池根（同 `LOAD_CONST`）。
- 绝对键经 `new_string` intern 驻留（weak root）。
- 命中分支取回的 `module` 经 `modules_` 根可达，`current_->push` 期间指针稳定（非移动 GC），无需守卫。

## 示例

设入口模块 `dir_` = `/proj`、`name_` = `main`（abs_path = `/proj/main.aria`，入口源根 =
`/proj` = 入口 `dir_`），stdlib = `/stdlib`，当前模块 `dir_` = `/proj/a`、`name_` = `b`
（abs_path = `/proj/a/b.aria`）：

| import 串 | 类别 | 基 | 绝对键 |
| :--- | :--- | :--- | :--- |
| `"lib/utils"` | 裸名 | source_roots | `/proj/lib/utils.aria`（入口根命中） |
| `"lib/utils.aria"` | 裸名，剥 `.aria` | source_roots | `/proj/lib/utils.aria` |
| `"./helper"` | 相对 | `dirname(cur)` = `/proj/a` = `dir_` | `/proj/a/helper.aria` |
| `"../x"` | 相对 | `/proj/a`，`..` 折叠 | `/proj/x.aria` |
| `"lib/./utils"` | 裸名，`weakly_canonical` 折 `.` | source_roots | `/proj/lib/utils.aria` |
| `"math"`（入口根无，stdlib 有） | 裸名，fall-through | source_roots | `/stdlib/math.aria` |

注意裸名不取当前模块目录：`/proj/a/b.aria` 里 `import "lib/math"` 解析到
`/proj/lib/math.aria`（相对入口源根），**不是** `/proj/a/lib/math.aria`。导入同目录兄弟
须用 `import "./math"` 显式相对。

## 当前边界与后续

- **已落地**：绝对键解析（`resolve_module` + `weakly_canonical` + 逐基 exists-check）、
  `source_roots` 播种（`[0]` 入口槽 cwd 占位 + `run()` 换入口 `dir_`、`[1..]` 配置根 stdlib）、
  `ObjModule::dir_`/`name_` + `abs_path()`（合成绝对路径，`dir_` 指针恒非空、内容可空 --
  `new_module` 默认 cwd）、`.aria` 后缀剥离、模块表命中复用（含循环导入半初始化）、循环导入语义、**未命中分支
  加载链路**（`load_module`：读文件 → `Compiler::compile` 编为被导入模块 CodeUnit（入口名 `<module>`、
  `set_entry`）→ 编译成功才入表；IMPORT 未命中分支以 `entry` 作普通 0 参函数调用进帧交主循环 run-once,模块体入口
  返回值恒为模块对象（编译器发射保证）,RETURN 通用写回 callee 槽,无递归 `dispatch_loop()`）。
- **解析缓存**：IMPORT 重复执行同一 specifier 需避免重复 stat。计划加一层缓存，键
  `(当前模块绝对目录, specifier ObjString*)` -> 已解析绝对键 `ObjString*`，命中即跳过磁盘。
  确切结构 / 存放位置留待实现时定。
- **stdlib 路径**：`<exe_dir>/../share/aria/lib` 为约定初值，确切路径与平台分流待定。
- **其余源根**：`-L` 标志、`ARIA_PATH` 环境变量等之后再加。
- **源根越界**：相对导入越出源根的检测未实现，留待加载层（`weakly_canonical` 折叠 `..` 后由加载层
  据源根列表判定，本原语不限路径范围）。

> 全景与各环节状态（词法 / 语法 / 编译器缺口 / 字节码 / VM / ObjModule / fs 原语 / 测试）
> 另见 [`import-handling-overview.md`](./import-handling-overview.md)。