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

## 项目状态

语言概览见 `README.md`，文法与语义约束见 `docs/grammar.txt`；实现契约和设计取舍分别见
`.agents/reference/` 与 `.agents/notes/`。本文件不另维护特性完成清单。

## 常用命令

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # 配置（Windows / 构建选项见 docs/building.md）
cmake --build build --target aria -j           # 构建解释器
cmake --build build --target aria_tests -j && ctest --test-dir build --output-on-failure --no-tests=error
clang++ -std=c++23 -I src -fsyntax-only <file>  # 单文件语法检查（-I src 必带）
clang-format -i <file>                         # 格式化（clang-format 23.1.3，与 CI 一致）
python3 tools/check_error_messages.py          # 报错文案机械检查（钩子壳 tools/hooks/pre-commit）
python3 tools/check_commit_msg.py <msg-file>   # commit 说明机械检查（钩子壳 tools/hooks/commit-msg）
python3 tools/check_agent_docs.py              # 检查文档链接、agent 文档路径/标识符/禁词
```

构建细节（三平台配置 / 构建选项 / 构建目录约定 / clangd）见 `docs/building.md`；测试组织与跑法见 `docs/testing.md`（技能 `aria-testing`）；跑基准见技能 `aria-bench`。

## 文档地图

真源一律住中立目录 `.agents/` 或 `docs/`。新检出时按下表和任务场景手动读取。自动发现依赖工具
支持并配置了对应的目录和 frontmatter（技能用 `description`，按路径加载的规则用 `paths:`）。
各工具的本地接线不入库。例如，在仓库根目录给使用 `.claude/rules/` 的工具接入 bytecode 参考：

```sh
mkdir -p .claude/rules
ln -s ../../.agents/reference/bytecode.md .claude/rules/bytecode.md
```

`.agents/` 按「拿它干什么」分三桶，写文档先判归属：

| 桶 | 只收 | 不收 |
| --- | --- | --- |
| `reference/` | 现在是什么：接口契约、类型地图、协议缝、跨类型规则、规格 | 历史沿革、被否方案、测量数字、路线 |
| `notes/` | 别再犯、别重做：踩坑挣下的不变式、被否方案的结论、测量纪律 | 现状复述（代码注释或 reference 已写的不要抄第二份） |
| `skills/` | 怎么做一件事：按序过的关卡与判据 | 与 `docs/` 同文的正文（只留判据 + 指针） |

同一规则或机制在所属文档维护，其他位置只保留使用所需的摘要和链接。reference 写现行契约，
notes 解释踩坑原因或被否方案；示意图和必要的不变量推导保留，避免复制到多处。

**模块参考 `.agents/reference/`（按源码路径选读）**：

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
- `compile/codegen.md`（编译器被否方案）/ `object/objstring.md`（字符串对象的已知拷贝风险）。

**场景技能 `.agents/skills/`（按工作场景触发）**：`aria-commit`（commit 说明规范，**每次 commit 前必读**）、`aria-bench`（性能测量纪律与报告规范）、`aria-error-messages`（报错文案速查）、`aria-language-change`（语言面改动同步清单）、`aria-testing`（测试组织与新增流程）、`aria-add-builtin`（加内建方法/静态原语/内建类的同步面）、`aria-bytecode-change`（加/改指令的同步面）。

**人类文档 `docs/`**：`grammar.txt`（手写文法规范，Lexer / Parser 实现应与其一致）、`guide/`（17 章教程，语言面验收面之一）、`building.md` / `testing.md`、`cpp-naming-convention.md`（C++ 代码风格规范本体）。

`tools/check_agent_docs.py` 检查行内 Markdown 相对链接的目标路径，以及 agent 文档里的仓库路径、
标识符与禁词。`Foo::bar` 只查两个标识符是否分别出现，不验证成员归属。CI 检查检出内容；
本地启用 Git 钩子后，pre-commit 检查暂存快照。

新增模块参考：建 `.agents/reference/<dir>.md`（frontmatter 带 name/description/paths）；新增规格放 `.agents/reference/specs/`，由对应模块参考指路；新增场景技能：建 `.agents/skills/<name>/SKILL.md`。本地软链按自己用的 agent 自建，不入库。

## 强制规范

### 输出与格式化

- **终端/日志输出统一用 `io::print` / `io::println`**（见 `util/io.hpp`，即对 `std::print`/`std::println` 的 using 别名），不要直接裸调 `std::print`/`std::cout`/`printf`。输出到 stderr 时用带 FILE* 的重载：`io::print(stderr, "...")`。
- **字符串格式化统一用 `std::format`**（`#include <format>`），不要混用 `sprintf`/`snprintf`/`fmt::format`/字符串拼接来构造格式化串。需要格式化结果字符串时用 `std::format("...", args...)`；需要直接输出时用 `io::println(std::format("...", args...))` 或 `io::print` 的格式串重载。
- **注释/文档破折号用 ASCII `--`（两个 hyphen），禁用 Unicode em-dash（U+2014）**：em-dash 被 Read 渲染成与 ASCII hyphen 难分的 `--`，会让 Edit 的 `old_string` 匹配失败。新增注释请保持用 ASCII `--`。

### 代码注释

- 只写代码本身难以表达、维护时必须知道的约束与原因：生命周期、GC 根、失效条件、非显然的顺序要求或语义边界。名字和代码已说清的内容不复述，不要求每个类型、函数或字段都有注释。
- 接口前置条件和失败约定放声明处，局部陷阱放相关实现旁，各写一处；派生类只说明与基类不同的约定。通用 GC 规则不在每个调用点重讲，特殊保活窗口才就地说明。
- 注释用短句，避免实现步骤流水、调用链清单、历史过程、被否方案和调试输出样例。跨文件机制与规格放 `.agents/reference/`，踩坑结论放 `.agents/notes/`，源码只留理解当前代码所需的要点。
- 测试注释解释场景为何特殊或为何这样构造，不逐句翻译断言；分组标题保持简短，不加装饰性分隔线。改代码时删除失效注释，不为保留注释而同步维护重复叙述。

### C++ 代码风格

完整强制规则集中在 [C++ 代码风格规范](docs/cpp-naming-convention.md)，修改 C++ 时按相关章节执行。
本文件仅保留关键要求：

- 类型用 PascalCase，函数、变量和参数用 snake_case；成员尾下划线、常量 `kPascalCase`、宏 ALL_CAPS。注释用中文。
- 名字默认完整单词，单字母和缩写仅限规范中的闭集白名单；同一概念全库同名。
- 使用 `src/type.hpp` 的别名，不直接写 `std::string` / `int` / `size_t` 等。
- GC 对象用指针，必非空服务或宿主、AST 节点用引用，小值按值；可空性与特殊参数按规范判定，所有权只经 `UPtr` 出现。
- Opt / Result 用条件隐式 bool、`*` / `->` 取值，禁 `.value()`；move 与显式 `has_value()` 按语境使用。
- 丢弃 nodiscard 返回值写 `std::ignore =` 并显式 include `<tuple>`；未用参数保留类型与名称，不加压制语句。
- 类初始化与局部声明的构造、赋值写法见规范的 Class Initialization / Variable Declarations，两处不另维护规则副本。

### 代码组织

- util 层多为 header-only（`inline`，常 `constexpr`，无 `.cpp`）；新增头文件要同步加进 `CMakeLists.txt` 的 `aria_core` 源列表。
- 头文件守卫用 `#ifndef ARIA_<MODULE>_HPP`。
- 命名空间分层：`aria` 为根，子模块 `aria::fs`/`aria::utf8`/`aria::util`/`aria::io` 等；`source_file` 相关类型在 `aria::src` 下（`String`/`Result` 等在 `aria` 下，引用需分别 using）。
- 私有细节进 `detail` 子命名空间,且**按文件/模块隔离,勿共用顶层 `aria::detail`**(多文件共用会互相污染):有模块命名空间的放进 `aria::<module>::detail`(如 `aria::fs::detail`/`aria::utf8::detail`/`aria::util::detail`),没有的用 `aria::detail::<module>`(如 `aria::detail::ht`/`aria::detail::ast`);`.cpp` 内的文件局部细节用匿名命名空间(`namespace { }`)。平台分流用 `src/sys.hpp` 的 `SYS_WINDOWS`/`SYS_LINUX`/`SYS_MACOS`/`SYS_FREEBSD` 等宏；Windows 含 `<windows.h>` 前加 `WIN32_LEAN_AND_MEAN` 和 `NOMINMAX`。

### 陷阱

- `SourceLoc` 借用 `SourceFile`，不拥有它；源文件借出的 `StringView`（`content()`/`Token::lexeme`）与 AST 节点烙入的 `SourceLoc` 须在 `SourceFile` 存活且地址不变期间使用--就位后再 tokenize，之后勿 move（SSO 短串 move 会改 data 地址）。`Error` 位置已烘焙、不受此约束。完整分析见 `.agents/reference/util.md`。
- 源码加载时 CRLF/CR 已归一化为 LF，`line`/`locate` 内部只按 `\n` 切行。

## 错误处理（src/error/）

原则常驻；Error 的静态工厂构造面 / 字段与尺寸 / ObjException 装箱载荷等结构细节见 `.agents/reference/error.md`，通道 2 见 `.agents/reference/runtime.md`「VM 异常通道」。

- **核心原则：内部用码，边界用 Error。** `ErrorCode`（1 字节纯码）供解释器**内部**判定（不变式断言 / 状态机分支 / 码到操作的映射）；`Error`（码 + 构造期一次性烘焙的完整消息，**不持 `SourceFile*`**、无悬空风险）是**边界与展示**的载体。
- **四条错误通道**（见 `.agents/reference/runtime.md`「VM 异常通道」）：① `Result<T, Error>` 返回--编译期可恢复错误的常规通道，VM 侧仅为未捕获出口的边界返回类型；② VM 自管异常状态（运行期主通道）--错误实体 `ObjException`（携码 + 烘焙消息、不含位置）存挂起错误寄存器，**运行期错误不就地构造 Error**，`Error` 仅在宿主 run 的未捕获失败出口物化、位置由逐帧 `at` 跟踪给出；③ `AriaException` 派生（C++ 异常）--仅用于 VM 之外跨 C++ 调用栈的边界，VM 主循环内不用；④ `fatal_error()`（`[[noreturn]]`）--Internal / Resource 类不可恢复错误。
- **文案一律英文 ASCII-only，句式按家族表收口**（强制）：同一语义只许一种形状（元数走 `AriaExeContext::arity_error` 唯一口）；ASSERT 文案不重复宏已打的文件/行/函数名。通则、句式家族表与机械门禁见 `.agents/reference/specs/error-messages.md` 与技能 `aria-error-messages`。

## Git 提交纪律（强制）

- **改动完成不自行 commit**：代码/文档改完、验证全绿后即向用户报告并停手，改动留在工作区等 review；用户明确说提交（「commit」/「提交吧」等）后才执行 `git commit`。不抢先 `git add` 备提交。
- 用户确认提交后，一次 review 通过的批次对应一次 commit；说明一律英文 ASCII。
- **每次 commit 前必读** `.agents/skills/aria-commit/SKILL.md`，执行其完整五步：暂存核范围、说明写入临时文件、运行说明检查、提交、复读说明。启用 `tools/hooks`；检查不得跳过。

## 同步义务

- 接口契约、跨模块约束、机制或文档定位发生变化时，更新对应 reference / specs，与实现同批交付。
  纯内部实现调整、格式化或注释措辞变化，相关文档仍准确时不要求机械改写。
- 新的踩坑原因或被否方案结论记入对应 notes，现行契约不在 notes 重写一遍。
- 文法或语言行为变化按 `aria-language-change` 技能同步文法、实现、语料和读者文档；只改受影响部分。
