# AGENTS.md

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，目标支持 Windows / Linux / macOS）：栈式字节码 VM + 单遍合一的字节码编译器 + mark-sweep GC，开发期即开 GC。本文件只收**跨阶段通用规则**与文档地图；模块细节一律按需加载（见下「文档地图」），不要在本文件里展开。

## 仓库布局

```
src/         入口 main.cpp / interpreter.cpp（CLI 分发），其余按层分目录：
  compile/   Lexer / Parser / CodeGen / Compiler，单遍产出字节码
  bytecode/  指令集（code.hpp）、CodeUnit、Disassembler
  runtime/   AriaVM 主循环与执行上下文；builtins/ 是内建函数与各类型的方法
  object/    Object 基类与全部 Obj* 子类型（字符串 / 函数 / 类 / 容器 / 迭代器等）
  memory/    GC 与内存基础设施（Allocator / HashTable / InternPool / StringBuilder 等）
  value/     Value 值表示（NaN-boxing，可切换 TagValue）、AriaArray / AriaHashTable
  error/     ErrorCode / Error / AriaException
  util/      fs / utf8 / source_file / io / cli 等工具（多为 header-only）
tests/       C++ GTest 白盒用例（tests/<module>/）+ aria 脚本语料（tests/language/）
bench/       性能基准：lexer / vm / hashtable 进程内微基准 + lang/ 源文件级基准
             （带 CPython / Node / Lua / Java 对照端口）+ profile/ 指令频度画像
docs/        guide/（教程）、grammar.txt（文法）、building / testing、
             cpp-naming-convention；索引见 docs/README.md
.agents/     agent 协作真源，按「拿它干什么」分三桶：reference/（现状参考 +
             specs/ 规格）、notes/（踩坑与测量笔记）、skills/（流程清单）；
             各 agent 的工具目录本地自建、不入库
tools/       开发脚本与 git 钩子（commit 说明 / 报错文案机械检查）
external/    isocline（REPL）/ googletest / mimalloc，vendored、构建零联网
```

## 当前进度

- **已落地**：util / value / error / compile / bytecode / memory 各层基础设施、GC（开发期即开）、Object 全部子类型、AriaVM（主循环 / 模块表与 IMPORT / 异常 / 闭包 / 类 / 协程：`Coroutine.create/resume/yield/status`、单循环切换模型、跨协程错误链式 unwind）、语言面补齐（值寄存器组、默认参数与 varargs、match 降糖、集合下标与切片、方法机制与迭代协议、解构含 rest、string/list/map 方法面、运算符重载的 11 个 dunder 钩子、`__str__` 显示钩子（经类面咨询口：实例字段出局、类值走元类链、容器调试位零执行））、字符串插值、显示虚函数退役（`to_string` 删除，显示定制唯一面 = `__str__`）。
- **待落地**：range 方法面（现仅 `iter`）；defer 善后为可选后续。
- 各特性语义决策见 `.agents/reference/` 与 `.agents/notes/` 的对应文档；`README.md` 是面向读者的项目介绍（语言概览 / 构建运行 / 项目结构），不承担进度记录。

## 常用命令

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # 配置（Windows / 构建选项见 docs/building.md）
cmake --build build --target aria -j           # 构建解释器
cmake --build build --target aria_tests -j && ctest --test-dir build --output-on-failure  # 测试
clang++ -std=c++23 -I src -fsyntax-only <file>  # 单文件语法检查（-I src 必带）
clang-format -i <file>                         # 格式化（clang-format 23.1.3，与 CI 一致）
python3 tools/check_error_messages.py          # 报错文案机械检查（钩子壳 tools/hooks/pre-commit）
python3 tools/check_commit_msg.py <msg-file>   # commit 说明机械检查（钩子壳 tools/hooks/commit-msg）
```

构建细节（三平台配置 / 构建选项 / 构建目录约定 / clangd）见 `docs/building.md`；测试组织与跑法见 `docs/testing.md`（技能 `aria-testing`）；跑基准见技能 `aria-bench`。

## 文档地图

真源一律住中立目录 `.agents/` 或 `docs/`，各 agent 的工具目录（`.claude/`、`.zcode/` 等）不入库：开发者按自己用的 agent 在本地建软链指向 `.agents/` 对应文件（技能按 frontmatter `description` 触发，模块参考按 `paths:` 匹配加载）。

`.agents/` 按「拿它干什么」分三桶，写文档先判归属：

| 桶 | 只收 | 不收 |
| --- | --- | --- |
| `reference/` | 现在是什么：接口契约、类型地图、协议缝、跨类型规则、规格 | 历史沿革、被否方案、测量数字、路线 |
| `notes/` | 别再犯、别重做：踩坑挣下的不变式、被否方案的结论、测量纪律 | 现状复述（代码注释或 reference 已写的不要抄第二份） |
| `skills/` | 怎么做一件事：按序过的关卡与判据 | 与 `docs/` 同文的正文（只留判据 + 指针） |

**模块参考 `.agents/reference/`（9 个，读到匹配源码路径时自动加载）**：

| 源码目录 | 参考文件 | 覆盖 |
| --- | --- | --- |
| `src/common.hpp`/`type.hpp`/`sys.hpp`/`aria.hpp`/`main.cpp`/`interpreter.hpp`/`interpreter.cpp` | `.agents/reference/core.md` | common(宏/USING_NANBOXING) / type(别名) / sys(平台宏) / aria(项目级定义) / main(解释器入口) / interpreter(CLI 分发核心) |
| `src/util/**` | `.agents/reference/util.md` | fs / utf8 / source_file / io / util / cli |
| `src/value/**` | `.agents/reference/value.md` | Value(NanBoxing/TagValue) / AriaArray / AriaHashTable |
| `src/error/**` | `.agents/reference/error.md` | ErrorCode / Error / AriaException |
| `src/compile/**` | `.agents/reference/compile.md` | Token / Lexer / ast / Parser / AstVisitor / FunctionCtx / ModuleCtx / CodeGen / Compiler |
| `src/bytecode/**` | `.agents/reference/bytecode.md` | code.hpp / CodeUnit / Disassembler |
| `src/runtime/**` | `.agents/reference/runtime.md` | FrameStack / AriaExeContext 执行上下文本体 / ObjMovement GC 对象壳 / Eval 驱动引擎 / AriaVM 宿主 / Builtins（含异常通道落地状态） |
| `src/object/**` | `.agents/reference/object.md` | Object / ObjString / ObjFnProto / ObjUpvalue / ObjFunction / ObjClass / ObjInstance / ObjBoundMethod / ObjList / ObjMap / ObjRange / ObjException / ObjModule / ObjMovement / iterator 族 |
| `src/memory/**` | `.agents/reference/memory.md` | Buffer / Array / Allocator / RawAlloc / HashTable / InternPool / ShellPool / GC |

**规格 `.agents/reference/specs/`（不自动加载，由对应模块参考指路后 Read）**：

- `instruction-set.md`：指令集、操作数编码、栈效应、lowering 蓝图。
- `lowering.md`：赋值类表达式的 locator-once lowering（复合赋值与前置 `++`/`--`）、LoopCtx 与 break/continue 回填。
- `import.md`：import 端到端链路与路径解析。
- `error-messages.md`：报错文案规范本体（机械门禁 `tools/check_error_messages.py`）。

**踩坑与测量笔记 `.agents/notes/`（不自动加载，对应特性重启前重读）**：

- `runtime/exception.md`（异常）/ `runtime/coroutine.md`（协程）/ `runtime/vm.md`（VM 结构与已否决方案）。
- `memory/ast-arena.md`（动 Parser/CodeGen 分配路径前先读）/ `compile/lexer.md`（动词法性能前先读）/ `.agents/notes/bench/lang.md`（基准测量与端口纪律）。

**场景技能 `.agents/skills/`（5 个，按工作场景触发）**：`aria-commit`（commit 说明规范，**每次 commit 前必读**）、`aria-bench`（性能测量纪律与报告规范）、`aria-error-messages`（报错文案速查）、`aria-language-change`（语言面改动同步清单）、`aria-testing`（测试组织与新增流程）。

**人类文档 `docs/`**：`grammar.txt`（手写文法规范，Lexer / Parser 实现应与其一致）、`guide/`（17 章教程，语言面验收面之一）、`building.md` / `testing.md`、`cpp-naming-convention.md`（命名规范本体）。

新增模块参考：建 `.agents/reference/<dir>.md`（frontmatter 带 name/description/paths）；新增规格放 `.agents/reference/specs/`，由对应模块参考指路；新增场景技能：建 `.agents/skills/<name>/SKILL.md`。本地软链按自己用的 agent 自建，不入库。

## 强制规范

### 输出与格式化

- **终端/日志输出统一用 `io::print` / `io::println`**（见 `util/io.hpp`，即对 `std::print`/`std::println` 的 using 别名），不要直接裸调 `std::print`/`std::cout`/`printf`。输出到 stderr 时用带 FILE* 的重载：`io::print(stderr, "...")`。
- **字符串格式化统一用 `std::format`**（`#include <format>`），不要混用 `sprintf`/`snprintf`/`fmt::format`/字符串拼接来构造格式化串。需要格式化结果字符串时用 `std::format("...", args...)`；需要直接输出时用 `io::println(std::format("...", args...))` 或 `io::print` 的格式串重载。
- **注释/文档破折号用 ASCII `--`（两个 hyphen），禁用 Unicode em-dash（U+2014）**：em-dash 被 Read 渲染成与 ASCII hyphen 难分的 `--`，会让 Edit 的 `old_string` 匹配失败。新增注释请保持用 ASCII `--`。

### 命名

只采用一套规则，不混 Google/LLVM/Unreal。优先级：可读性 > 一致性 > 简洁性。注释用中文。（完整规则见 `docs/cpp-naming-convention.md`，本节为其摘要。）

- 类型 / struct / enum class / 别名 / concept / 模板类型参数 / 枚举值：`PascalCase`
- 函数 / 变量 / 参数：`snake_case`
- 非静态成员：`snake_case_`（尾下划线）
- `constexpr`/`static const` 常量：`kPascalCase`
- 命名空间：`lowercase`；宏：`ALL_CAPS`
- 布尔用 `is_`/`has_`/`can_`/`should_`/`was_`/`needs_` 前缀
- 禁 Hungarian（`m_`）、禁保留标识符（`_Foo`）、禁宏风格常量（`MAX_SIZE`）

参数传递（指针 / 引用 / 按值的选择）同属强制，规则见 `docs/cpp-naming-convention.md`「Parameter Passing」节。要点：**所有权只经 `UPtr` 出现**；object 层 GC 对象类型（`Object`/`Obj*`）一律按指针；服务/宿主（`GC`/`AriaVM`/`SourceFile` 等借用期必非空）按引用；仅可空（`nullptr` 合法）、位置/槽位（`Value*`）、dyn_cast 查询族与容器分配器注入（`Alloc*`）用指针；AST 节点非空借用按引用（`visitXxxNode(XxxNode&)`）；小值类型（`Value`/标量/`SourceLoc`/`StringView`/`Span`）按值，`const` 写在定义处（不改参契约，纯声明不写）。

变量与参数名的语义同属强制，规则见 `docs/cpp-naming-convention.md`「Variable & Parameter Names」节。要点：**默认完整单词**，参数零单字母（下标 `index`；纯数量参数可用 `n`），禁臆造截断（`mod`/`tok`/`res` 一类，截断会撞词且 grep 不可及）；单字母与缩写只来自成文白名单--`i`/`j`/`k`（循环计数）、`n`（数量，「n 个 xx」）、`c`（逐字符扫描局部）、`ch`（字符参数）、`lhs`/`rhs`（操作数）、`loc`（随 `SourceLoc` 短名）、`src`（源文件/源码文本，随 `SourceFile`）、`cp`（码点）、`expr`/`stmt`（AST 节点）、`ctx`（执行上下文），清单是闭集、新条目先入表再用；同一概念全库同名。

返回值处置同属强制，规则见 `docs/cpp-naming-convention.md`「Nodiscard 与结果丢弃」节。要点：丢弃 `[[nodiscard]]` 返回值写 `std::ignore = f(...)`，不写 `(void) f(...)`（含 `AriaVM::fail`，其惯用出口仍是 `return vm.fail(...)`）；被调函数未标 `[[nodiscard]]` 时调用语句前不留 `(void)`，那是纯装饰、直接删；压制未用**变量**的 `(void) x;` 保留，压制未用**参数**一律不许（签名照常带类型带参数名）；用 `std::ignore` 的 TU 显式 `#include <tuple>`。

### 类型（src/type.hpp）

**不要直接用 `std::string`/`int`/`size_t` 等**，一律用 `src/type.hpp` 的别名（`i8..i64`/`u8..u64`/`isize`/`usize`/`f32`/`f64`/`String`/`StringView`/`List`/`Vector`/`HashMap`/`HashSet`/`Stack`/`Pair`/`Tuple`/`Span`/`UPtr`/`SPtr`/`Result<T,E>`/`Opt<T>`；完整映射见该头与 `.agents/reference/core.md`）。错误处理倾向 `Result` 返回而非抛异常。`Opt`/`Result` 的判断/取值/move 按语境各定一式（条件隐式 bool、取值 `*`/`->` 禁 `.value()`、终局 move、断言显式 `has_value()`），规则见 `docs/cpp-naming-convention.md`「Optional/Result 用法」节。

### 代码组织

- util 层多为 header-only（`inline`，常 `constexpr`，无 `.cpp`）；新增头文件要同步加进 `CMakeLists.txt` 的 `aria_core` 源列表。
- 头文件守卫用 `#ifndef ARIA_<MODULE>_HPP`。
- 命名空间分层：`aria` 为根，子模块 `aria::fs`/`aria::utf8`/`aria::util`/`aria::io` 等；`source_file` 相关类型在 `aria::src` 下（`String`/`Result` 等在 `aria` 下，引用需分别 using）。
- 私有细节进 `detail` 子命名空间,且**按文件/模块隔离,勿共用顶层 `aria::detail`**(多文件共用会互相污染):有模块命名空间的放进 `aria::<module>::detail`(如 `aria::fs::detail`/`aria::utf8::detail`/`aria::util::detail`),没有的用 `aria::detail::<module>`(如 `aria::detail::ht`/`aria::detail::ast`);`.cpp` 内的文件局部细节用匿名命名空间(`namespace { }`)。平台分流用 `src/sys.hpp` 的 `SYS_WINDOWS`/`SYS_LINUX`/`SYS_MACOS`/`SYS_FREEBSD` 等宏；Windows 含 `<windows.h>` 前加 `WIN32_LEAN_AND_MEAN` 和 `NOMINMAX`。

### 类初始化

- **简单类**（纯数据聚合的小结构体，如 `src/util/source_file.hpp` 中的 `LineCol`）：字段少、无逻辑、初始化无依赖，用类内默认成员初始化即可，不必上构造函数。
- **复杂类**（带逻辑或多步/有依赖的初始化，如 `Token`/`Lexer`/`SourceFile`/`GC`）：成员声明处**不写默认值**（或仅写无争议空态如 `= nullptr`/`= 0`），所有初始化统一收敛进构造函数（初始化列表 + 函数体），不要把初始化散落到各字段声明处。
- 构造函数初始化列表统一用**大括号**写每个成员，形如 `FooClass : mem1{...}, mem2{...}, ... {}`（不用小括号 `mem1(...)`）。大括号即统一初始化语法，会禁止窄化转换，也与项目内其它初始化（如 `SourceFile` 构造）风格一致。

### 变量声明风格

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

### 陷阱

- `SourceFile` 以指针传入（非拥有）：其借出的 `StringView`（`content()`/`Token::lexeme`）与 AST 节点烙入的 `SourceLoc` 须在 `SourceFile` 存活且地址不变期间使用--就位后再 tokenize，之后勿 move（SSO 短串 move 会改 data 地址）。`Error` 位置已烘焙、不受此约束。完整分析见 `.agents/reference/util.md`。
- 源码加载时 CRLF/CR 已归一化为 LF，`line`/`locate` 内部只按 `\n` 切行。

## 错误处理（src/error/）

原则常驻；Error 的静态工厂构造面 / 字段与尺寸 / ObjException 装箱载荷等结构细节见 `.agents/reference/error.md`，通道 2 的落地状态见 `.agents/reference/runtime.md`「VM 异常通道」（读对应源码时自动加载）。

- **核心原则：内部用码，边界用 Error。** `ErrorCode`（1 字节纯码）供解释器**内部**判定（不变式断言 / 状态机分支 / 码到操作的映射）；`Error`（码 + 构造期一次性烘焙的完整消息，**不持 `SourceFile*`**、无悬空风险）是**边界与展示**的载体。
- **四条错误通道**（见 `.agents/reference/runtime.md`「VM 异常通道」）：① `Result<T, Error>` 返回--编译期可恢复错误的常规通道，VM 侧仅为未捕获出口的边界返回类型；② VM 自管异常状态（运行期主通道）--错误实体 `ObjException`（携码 + 烘焙消息、不含位置）存挂起错误寄存器，**运行期错误不就地构造 Error**，`Error` 仅在 `unwind()` 全未命中的出口物化、位置由逐帧 `at` 跟踪给出；③ `AriaException` 派生（C++ 异常）--仅用于 VM 之外跨 C++ 调用栈的边界，VM 主循环内不用；④ `fatal_error()`（`[[noreturn]]`）--Internal / Resource 类不可恢复错误。
- **文案一律英文 ASCII-only，句式按家族表收口**（强制）：同一语义只许一种形状（元数走 `AriaVM::arity_error` 唯一口）；ASSERT 文案不重复宏已打的文件/行/函数名。通则、句式家族表与机械门禁见 `.agents/reference/specs/error-messages.md` 与技能 `aria-error-messages`。

## Git 提交纪律（强制）

- **改动完成不自行 commit**：代码/文档改完、验证全绿后即向用户报告并停手，改动留在工作区等 review；用户明确说提交（「commit」/「提交吧」等）后才执行 `git commit`。不抢先 `git add` 备提交。
- 用户确认提交后：一批一 commit（一次 commit = 一次 review 通过的批），提交说明按 `.agents/skills/aria-commit/SKILL.md` 成文，**一律英文 ASCII-only**（禁 CJK / 全角标点 / Unicode em-dash，破折号用 `--`）：标题行 `<type>(<scope>): <subject>`（type 固定九类枚举、祈使句动词原形、无句号、软目标 ≤ 50 列硬上限 72 列）+ **正文默认不写**（只在标题与 diff 说不清 why 时写 1~3 句散文、≤ 4 行，不分段标签、不列 bullet）+ 可选末行 `Tests: ...`（跑过测试就写实际结果）。只写本次改了什么：实现步骤流水、评审/决策注记、被否方案推导、实测数字、文档指针、规划词汇一律不进说明（各一条反例见规范 §3）。
- **落笔前加载技能 `aria-commit` 并走规范 §4 五步**，不凭记忆：① `git add -u` 后 `git diff --cached --stat` 核范围；② 说明写进临时文件（`/tmp/commit_msg.txt` 一类），不用 `-m` 随手一句；③ 跑 `python3 tools/check_commit_msg.py <file>`，必须返回 0；④ `git commit -F <file>`（改已有说明用 `git commit --amend -F <file>`，仅限未推送）；⑤ `git log -1 --format=%B` 复读，对规范 §7 自检清单逐条过（「正文是不是在讲过程」脚本判不出）。第 ③ 步另有 `commit-msg` 钩子（`tools/hooks/`，经 `git config core.hooksPath tools/hooks` 启用，同一次启用也挂 `pre-commit` 跑文案检查）在落库时自动兜底，违规直接拒提交。

## 同步义务

- 改模块代码：同步更新对应 `.agents/reference/<dir>.md`，随代码同批落库。
- 改规格或机制：同步 `.agents/reference/specs/` 对应规格；踩坑结论进 `.agents/notes/`，随代码同批落库。
- 改文法：同步 `docs/grammar.txt` + 实现 + 测试；语言面改动的完整同步清单见技能 `aria-language-change`（tests/language 语料、docs/guide 章节、README 特性表一并同步）。
