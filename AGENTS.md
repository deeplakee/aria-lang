# AGENTS.md

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，目标支持 Windows / Linux / macOS）：栈式字节码 VM + 单遍合一的字节码编译器 + mark-sweep GC，开发期即开 GC。本文件只收**跨阶段通用规则**与文档索引；模块细节一律按需加载（见下「文档与参考」），不要在本文件里展开。

## 当前进度

- **已落地**：util / value / error / compile / bytecode / memory 各层基础设施、GC（开发期即开）、Object 全部子类型、AriaVM M1-M5（主循环 / 模块表与 IMPORT / 异常 / 闭包 / 类）、P0 语言面补齐（值寄存器组、默认参数与 varargs、match 降糖、集合下标与切片、方法机制与迭代协议、解构含 rest、string/list/map 方法面、运算符重载的 11 个 dunder 钩子）。
- **待落地**：M6 协程（单循环切换模型见 `vm-design.md` §4.9）；range 方法面按需另批（现仅 `iter`）；defer 善后为可选后续，不绑定里程碑。
- 里程碑级细节见 `.claude/reference/runtime/vm-design.md` §6 路线表，各特性语义决策见对应 `.claude/reference/` 文档；`README.md` 是面向读者的项目介绍（语言概览 / 构建运行 / 项目结构），不承担进度记录。

## 文档与参考（按需加载）

### 模块参考 `.claude/rules/<dir>.md`（9 个，自动加载）

frontmatter 带 `paths:`，读到匹配源码路径时**自动加载**，不读则不进上下文：

| 源码目录 | 参考文件 | 覆盖 |
| --- | --- | --- |
| `src/common.hpp`/`type.hpp`/`sys.hpp`/`aria.hpp`/`main.cpp`/`interpreter.hpp`/`interpreter.cpp` | `.claude/rules/core.md` | common(宏/USING_NANBOXING) / type(别名) / sys(平台宏) / aria(项目级定义) / main(解释器入口) / interpreter(CLI 分发核心) |
| `src/util/**` | `.claude/rules/util.md` | fs / utf8 / source_file / io / util / cli |
| `src/value/**` | `.claude/rules/value.md` | Value(NanBoxing/TagValue) / AriaArray / AriaHashTable |
| `src/error/**` | `.claude/rules/error.md` | ErrorCode / Error / AriaException |
| `src/compile/**` | `.claude/rules/compile.md` | Token / Lexer / ast / Parser / AstVisitor / FunctionCtx / ModuleCtx / CodeGen / Compiler |
| `src/bytecode/**` | `.claude/rules/bytecode.md` | code.hpp / CodeUnit / Disassembler |
| `src/runtime/**` | `.claude/rules/runtime.md` | FrameStack / Movement / AriaVM / Builtins（含异常通道落地状态） |
| `src/object/**` | `.claude/rules/object.md` | Object / ObjString / ObjFunction / ObjUpvalue / ObjClosure / ObjClass / ObjInstance / ObjBoundMethod / ObjList / ObjMap / ObjRange / ObjNativeFn / ObjException / ObjModule / iterator 族 |
| `src/memory/**` | `.claude/rules/memory.md` | Buffer / Array / Allocator / HashTable / InternPool / GC |

### 深度设计文档 `.claude/reference/`（不自动加载，需要时 Read）

- `bytecode/bytecode-instruction-set.md` -- 指令集规格（功能 / 操作数位宽 / 栈效应 / 反汇编格式）。
- `runtime/vm-design.md` -- AriaVM / 执行上下文设计与 M1-M6 分阶段路线。
- `runtime/import-handling-overview.md` / `import-path-resolution.md` -- import 端到端处理与路径解析细节。
- `runtime/exception-implementation-pitfalls.md` -- M3 异常踩坑归档（含 finally 裁撤与 defer 后继说明；异常相关特性重启前重读）。
- `runtime/class-implementation-pitfalls.md` -- M5 类踩坑归档（bound 缓存已取消 = 读路径每次访问现场绑定、peek-不弹栈白色对象发布、`init_` 写点（ctor 自 super 派生 + `set_field` 同步）、Locate 合流栈泄漏；类相关特性重启前重读）。
- `compile/compound-assignment-lowering.md` / `loopctx.md` -- 复合赋值 lowering、LoopCtx 与 break/continue 回填机制。
- `compile/lexer-notes.md` -- 词法层实测数字与已实测否决的优化清单（动词法性能前先读；含测量纪律与尚未纳入基准的输入形态）。

### 语言文法 `docs/grammar.txt`

手写文法规范。Lexer / Parser 的实现应与其保持一致，改文法时同步更新相关代码与测试。（注意「语言规范」与「解析器实现蓝图」两种视角可能存在差别，但描述的语法规则一致。）

### 技能与软链现状

- `.zcode/skills/aria-<dir>/SKILL.md` **软链** → `../../../.claude/rules/<dir>.md`（9 个模块相对软链，与规则文件同名，`core.md` 对应技能 `aria-core`）：ZCode 把同一份规则文件暴露为技能、按 frontmatter `description` 自动触发，`paths:` 字段仅 Claude Code 消费。
- `.zcode/skills/aria-commit/SKILL.md` 收 commit 说明规范（frontmatter 供技能注册、正文即规范），**每次 commit 前必读**。
- 新增模块规则：建 `.claude/rules/<dir>.md`（frontmatter 带 name/description/paths），并补一条 `.zcode/skills/aria-<dir>/SKILL.md` 相对软链。

> 同步义务：改模块代码时同步更新对应 `.claude/rules/<dir>.md`；改设计时同步 `.claude/reference/` 对应文档；改文法时同步 `docs/grammar.txt` + 代码 + 测试。

## 构建

- CMake ≥ 3.20，C++23。clang++ / clang-format / clangd 均已在 PATH 中，可直接调用。在 `build` 目录中进行构建（配置 / 构建 / 测试命令见 `README.md`「构建与运行」「测试与基准」）。
- 单文件语法检查必须带 `-I src`，否则 `common.hpp`/`type.hpp` 找不到：`clang++ -std=c++23 -I src -fsyntax-only <file>`。
- 验证 TagValue 值表示（`common.hpp` 的 `USING_NANBOXING` 关闭路径）：另配独立 build 目录并配 `-DARIA_USE_TAGVALUE=ON` 全量构建 + ctest（命令同 README，构建目录换 `build/tagvalue`）。
- 依赖 `external/isocline`（REPL）。IO 通过封装 `std::print`/`std::println` 实现。

## Git 提交纪律（强制）

- **改动完成不自行 commit**：代码/文档改完、验证全绿后即向用户报告并停手，改动留在工作区等 review；用户明确说提交（「commit」/「提交吧」等）后才执行 `git commit`。不抢先 `git add` 备提交。
- 用户确认提交后：一批一 commit（一次 commit = 一次 review 通过的批），提交说明按根目录 `.zcode/skills/aria-commit/SKILL.md` 成文，**一律英文 ASCII-only**（禁 CJK / 全角标点 / Unicode em-dash，破折号用 `--`）：标题行 `<type>(<scope>): <subject>`（type 固定九类枚举、祈使句动词原形、无句号、软目标 ≤ 50 列硬上限 72 列）+ **正文默认不写**（只在标题与 diff 说不清 why 时写 1~3 句散文、≤ 4 行，不分段标签、不列 bullet）+ 可选末行 `Tests: ...`（跑过测试就写实际结果）。只写本次改了什么：实现步骤流水、评审/决策注记、被否方案推导、实测数字、文档指针、规划词汇一律不进说明（各一条反例见规范 §3）。
- **落笔前加载技能 `aria-commit`（= `.zcode/skills/aria-commit/SKILL.md` 本体）并走规范 §4 五步**，不凭记忆：① `git add -u` 后 `git diff --cached --stat` 核范围；② 说明写进临时文件（`/tmp/commit_msg.txt` 一类），不用 `-m` 随手一句；③ 跑 `python3 tools/check_commit_msg.py <file>`，必须返回 0；④ `git commit -F <file>`（改已有说明用 `git commit --amend -F <file>`，仅限未推送）；⑤ `git log -1 --format=%B` 复读，对规范 §7 自检清单逐条过（「正文是不是在讲过程」脚本判不出）。第 ③ 步另有 `commit-msg` 钩子（`tools/hooks/`，经 `git config core.hooksPath tools/hooks` 启用）在落库时自动兜底，违规直接拒提交。

## 输出与格式化（强制）

- **终端/日志输出统一用 `io::print` / `io::println`**（见 `util/io.hpp`，即对 `std::print`/`std::println` 的 using 别名），不要直接裸调 `std::print`/`std::cout`/`printf`。输出到 stderr 时用带 FILE* 的重载：`io::print(stderr, "...")`。
- **字符串格式化统一用 `std::format`**（`#include <format>`），不要混用 `sprintf`/`snprintf`/`fmt::format`/字符串拼接来构造格式化串。需要格式化结果字符串时用 `std::format("...", args...)`；需要直接输出时用 `io::println(std::format("...", args...))` 或 `io::print` 的格式串重载。
- **注释/文档破折号用 ASCII `--`（两个 hyphen），禁用 Unicode em-dash（U+2014）**：em-dash 被 Read 渲染成与 ASCII hyphen 难分的 `--`，会让 Edit 的 `old_string` 匹配失败。新增注释请保持用 ASCII `--`。

## 错误处理（src/error/）

原则常驻；Error 的静态工厂构造面 / 字段与尺寸 / ObjException 装箱载荷等结构细节见 `.claude/rules/error.md`，通道 2 的落地状态见 `.claude/rules/runtime.md`「VM 异常通道」（读对应源码时自动加载）。

- **核心原则：内部用码，边界用 Error。** `ErrorCode`（1 字节纯码）供解释器**内部**判定（不变式断言 / 状态机分支 / 码到操作的映射）；`Error`（码 + 构造期一次性烘焙的完整消息，**不持 `SourceFile*`**、无悬空风险）是**边界与展示**的载体。
- **四条错误通道**（设计见 vm-design.md §4.5-§4.8）：
  1. **`Result<T, Error>` 返回**：编译期可恢复错误的常规通道（`Lexer::tokenize` / `Parser::parse` 恢复式收集、`Compiler::compile` 单错）。VM 侧 `run()` 的 `Result` 仅为未捕获出口的边界返回类型，dispatch_loop 内部不逐站传播。
  2. **VM 自管异常状态（运行期主通道）**：throw/catch 与 VM 检测到的运行时错误统一走 VM 机制，错误实体是 `ObjException`（携码 + 烘焙消息，**不含位置**），装箱为 Value 存入当前执行上下文的挂起错误寄存器。故**运行期错误不就地构造 Error**：`Error` 仅在 `unwind()` 全未命中的出口物化，位置由该出口烘焙的逐帧 `at` 堆栈跟踪给出。
  3. **`AriaException` 派生**（C++ 异常）：仅用于 VM 之外、跨 C++ 调用栈的边界（Parser / CodeGen 深层 `fail()` 抛出、顶层 catch 翻译为 `Result`）；VM 主循环内不用（不跨 C++ 栈且是热路径）。
  4. **`fatal_error()`**（`[[noreturn]]`）：Internal / Resource 类不可恢复错误（`Unreachable`/`OutOfMemory`），打印 stderr 后 `std::exit(1)`。

## 工具

- **clang-format**（根目录 `.clang-format`，LLVM 风格 / 4 空格 / 120 列 / 命名空间全缩进）：`clang-format -i <file>` 原地格式化。编辑器保存时自动重排（如 `auto p`->`const auto p`）是项目风格，不要回退。
- **tools/check_commit_msg.py**（python3，无依赖，不参与构建）：commit 说明机械检查，`python3 tools/check_commit_msg.py <message-file>` 判 ASCII-only、标题格式与列宽、正文长度与形态（散文而非段标签/bullet）、`Tests:` 行位置与规范 §3 禁写关键词，有违规返非零并打印改法；`tools/hooks/commit-msg` 是它的 git 钩子壳（`git config core.hooksPath tools/hooks` 启用后每次 commit 自动跑，违规拒提交）。按「Git 提交纪律」五步的第 ③ 步每次提交前手跑一遍。
- **clangd**：读 `compile_commands.json`（CMake `EXPORT_COMPILE_COMMANDS` 生成）。注意 `compile_commands.json` 只含 `.cpp`/`.c`--header-only 头文件**不被任何编译 TU（直接或传递）include** 时会因拿不到编译参数报类型未定义假错；已被传递 include 的头 clangd 能推断参数，自含头即可。疑似假错以 `clang++ -std=c++23 -I src -fsyntax-only` 实编译为准；根治：尽早让某 .cpp include 一次（仅对确实不可达的头需要）。

## 命名（强制）

只采用一套规则，不混 Google/LLVM/Unreal。优先级：可读性 > 一致性 > 简洁性。注释用中文。（完整规则见根目录 `CPP_Naming_Convention.md`，本节为其摘要。）

- 类型 / struct / enum class / 别名 / concept / 模板类型参数 / 枚举值：`PascalCase`
- 函数 / 变量 / 参数：`snake_case`
- 非静态成员：`snake_case_`（尾下划线）
- `constexpr`/`static const` 常量：`kPascalCase`
- 命名空间：`lowercase`；宏：`ALL_CAPS`
- 布尔用 `is_`/`has_`/`can_`/`should_`/`was_`/`needs_` 前缀
- 禁 Hungarian（`m_`）、禁保留标识符（`_Foo`）、禁宏风格常量（`MAX_SIZE`）

参数传递（指针 / 引用 / 按值的选择）同属强制，规则见根目录 `CPP_Naming_Convention.md`「Parameter Passing」节。要点：**所有权只经 `UPtr` 出现**；object 层 GC 对象类型（`Object`/`Obj*`）一律按指针；服务/宿主（`GC`/`AriaVM`/`SourceFile` 等借用期必非空）按引用；仅可空（`nullptr` 合法）、位置/槽位（`Value*`）、dyn_cast 查询族与容器分配器注入（`Alloc*`）用指针；AST 节点非空借用按引用（`visitXxxNode(XxxNode&)`）；小值类型（`Value`/标量/`SourceLoc`/`StringView`/`Span`）按值，`const` 写在定义处（不改参契约，纯声明不写）。

变量与参数名的语义同属强制，规则见根目录 `CPP_Naming_Convention.md`「Variable & Parameter Names」节。要点：**默认完整单词**，参数零单字母（下标 `index`；纯数量参数可用 `n`），禁臆造截断（`mod`/`tok`/`res` 一类，截断会撞词且 grep 不可及）；单字母与缩写只来自成文白名单--`i`/`j`/`k`（循环计数）、`n`（数量，「n 个 xx」）、`c`（逐字符扫描局部）、`ch`（字符参数）、`lhs`/`rhs`（操作数）、`loc`（随 `SourceLoc` 短名）、`src`（源文件/源码文本，随 `SourceFile`）、`cp`（码点）、`expr`/`stmt`（AST 节点）、`ctx`（执行上下文），清单是闭集、新条目先入表再用；同一概念全库同名。

返回值处置同属强制，规则见根目录 `CPP_Naming_Convention.md`「Nodiscard 与结果丢弃」节。要点：丢弃 `[[nodiscard]]` 返回值写 `std::ignore = f(...)`，不写 `(void) f(...)`（含 `AriaVM::fail`，其惯用出口仍是 `return vm.fail(...)`）；被调函数未标 `[[nodiscard]]` 时调用语句前不留 `(void)`，那是纯装饰、直接删；压制未用**变量**的 `(void) x;` 保留，压制未用**参数**一律不许（签名照常带类型带参数名）；用 `std::ignore` 的 TU 显式 `#include <tuple>`。

## 类型（src/type.hpp）

**不要直接用 `std::string`/`int`/`size_t` 等**，一律用 `src/type.hpp` 的别名（`i8..i64`/`u8..u64`/`isize`/`usize`/`f32`/`f64`/`String`/`StringView`/`List`/`Vector`/`HashMap`/`HashSet`/`Stack`/`Pair`/`Tuple`/`Span`/`UPtr`/`SPtr`/`Result<T,E>`/`Opt<T>`；完整映射见该头与 `.claude/rules/core.md`）。错误处理倾向 `Result` 返回而非抛异常。`Opt`/`Result` 的判断/取值/move 按语境各定一式（条件隐式 bool、取值 `*`/`->` 禁 `.value()`、终局 move、断言显式 `has_value()`），规则见 `CPP_Naming_Convention.md`「Optional/Result 用法」节。

## 代码组织

- util 层多为 header-only（`inline`，常 `constexpr`，无 `.cpp`）；新增头文件要同步加进 `CMakeLists.txt` 的 `aria_core` 源列表。
- 头文件守卫用 `#ifndef ARIA_<MODULE>_HPP`。
- 命名空间分层：`aria` 为根，子模块 `aria::fs`/`aria::utf8`/`aria::util`/`aria::io` 等；`source_file` 相关类型在 `aria::src` 下（`String`/`Result` 等在 `aria` 下，引用需分别 using）。
- 私有细节进 `detail` 子命名空间,且**按文件/模块隔离,勿共用顶层 `aria::detail`**(多文件共用会互相污染):有模块命名空间的放进 `aria::<module>::detail`(如 `aria::fs::detail`/`aria::utf8::detail`/`aria::util::detail`),没有的用 `aria::detail::<module>`(如 `aria::detail::ht`/`aria::detail::ast`);`.cpp` 内的文件局部细节用匿名命名空间(`namespace { }`)。平台分流用 `src/sys.hpp` 的 `SYS_WINDOWS`/`SYS_LINUX`/`SYS_MACOS`/`SYS_FREEBSD` 等宏；Windows 含 `<windows.h>` 前加 `WIN32_LEAN_AND_MEAN` 和 `NOMINMAX`。

## 类初始化

- **简单类**（纯数据聚合的小结构体，如 `src/util/source_file.hpp` 中的 `LineCol`）：字段少、无逻辑、初始化无依赖，用类内默认成员初始化即可，不必上构造函数。
- **复杂类**（带逻辑或多步/有依赖的初始化，如 `Token`/`Lexer`/`SourceFile`/`GC`）：成员声明处**不写默认值**（或仅写无争议空态如 `= nullptr`/`= 0`），所有初始化统一收敛进构造函数（初始化列表 + 函数体），不要把初始化散落到各字段声明处。
- 构造函数初始化列表统一用**大括号**写每个成员，形如 `FooClass : mem1{...}, mem2{...}, ... {}`（不用小括号 `mem1(...)`）。大括号即统一初始化语法，会禁止窄化转换，也与项目内其它初始化（如 `SourceFile` 构造）风格一致。

## 变量声明风格（强制）

声明局部变量时，按对象用途选择写法（构造用大括号 `{...}`、不用小括号，与「类初始化」节一致；从表达式/已有值赋值用 `=`）：

| 类别 | 推荐写法 | 原因 |
| :--- | :--- | :--- |
| RAII Guard / Scope 标记 | `T val{...}` | 类型=契约，变量=行为标记，非值语义 |
| 基础数据类型（短类型名，如 `usize`/`i32`/`f64`/`bool`） | `T val = expr` / `T val{...}` | 类型名短，显式写出比 `auto` 直白，不淹没变量名 |
| 普通数据对象 | `auto val = T{...}` | 变量名优先，类型在右侧自文档化 |
| 工厂/推导结果 | `auto val = expr` | 类型由表达式决定，手写无意义 |
| 类型转换结果（`static_cast`/`reinterpret_cast` 等） | `auto val = expr` | 类型已在 cast 的 `<T>` 写明，左侧再写重复 |
| 类型极长的数据对象 | `auto val = T{...}` | 避免变量名被淹没 |
| 公共 API / 接口边界 | `T val{...}` | 类型是对外契约的一部分 |

- 右侧是 `static_cast<T>` 等类型转换时左侧统一用 `auto`，即使 `T` 是基础类型也不套用「基础数据类型」行。
- 构造用 `{}` 的例外：大括号会触发 `initializer_list` 窄化或歧义时用小括号，如 `String(size, '\0')` 不能写 `String{size, '\0'}`（`size` 窄化为 `char` 报错）。
- `auto val = expr` 仅用于类型完全由右侧表达式决定的场合（工厂返回、`make_*`、推导），不要为省事对显式构造也用。

## 陷阱

- `SourceFile` 以指针传入（非拥有）：其借出的 `StringView`（`content()`/`Token::lexeme`）与 `Token::loc_` 内的 `SourceLoc` 须在 `SourceFile` 存活且地址不变期间使用--就位后再 tokenize，之后勿 move（SSO 短串 move 会改 data 地址）。`Error` 位置已烘焙、不受此约束。完整分析见 `.claude/rules/util.md`。
- 源码加载时 CRLF/CR 已归一化为 LF，`line`/`locate` 内部只按 `\n` 切行。

## 测试

用 Google Test，位于 `tests/`（按 `tests/<module>/` 分目录）。GTest 通过 `FetchContent_Declare`（CMakeLists.txt 末尾）下载，配置时联网拉取 `v1.14.0`。配置 / 构建 / 运行命令见 `README.md`「测试与基准」。

- 新增测试：在 `tests/<module>/` 加 `test_<module>.cpp`，并在 `tests/CMakeLists.txt` 的 `aria_tests` 源列表里登记。
- 测试链接 `aria_core` + `gtest_main`，用 `gtest_discover_tests` 注册到 ctest。
- 临时文件用 `testing::TempDir()`（gtest 提供）写入，测试结束自动清理。
- `tests/language/` 另有 aria 自己写的脚本语料（正向 assert 收口、负向钉错误码，端到端驱动解释器，兼作 GC 压力网），跑法与约束见 `tests/language/README.md`；改语言面时它同样是验收面。

`build/` 与 `cmake-build-*` 均已加入 `.gitignore`。
