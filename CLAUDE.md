# CLAUDE.md

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，目标支持 Windows / Linux / macOS）：栈式字节码 VM + 单遍合一的字节码编译器 + mark-sweep GC，开发期即开 GC。本文件只收**跨阶段通用规则**与文档索引；模块细节一律按需加载（见下「文档与参考」），不要在本文件里展开。

## 当前进度

- **已落地**：util / value / error / compile 层（Token / Lexer / AST / Parser / AstVisitor / CodeGen / Compiler）、bytecode 层（OpCode X-Macro 单一事实源表 + CodeUnit / Disassembler 表驱动解码 + 异常记录表小节）、GC Phase 1 + 2、Object 子类型 ObjString / ObjFunction / ObjNativeFn / ObjModule / ObjException / ObjClosure / ObjUpvalue、AriaVM M1 主循环 + M2（模块表 / 源根 / `DEF/LOAD/STORE_GLOBAL` / builtins type·len·str·assert / `IMPORT` 磁盘加载全链 / 运行期报错带 `path:line: ` 位置标注）+ **M3 异常 try/catch/throw**（统一寄存器通道 + `unwind` 查异常记录表派发 / 跨帧 unwind / `THROW` 原值保类型 / re-throw 保码 / 未捕获逐帧堆栈跟踪；dispatch_loop 直报 Result 形态已全部退役）+ **M4 闭包**（「捕获即引用」语义：ObjClosure/ObjUpvalue、open upvalue 按槽址降序开链 + 值栈增长第三类重绑、`CLOSURE/LOAD_UPVALUE/STORE_UPVALUE/CLOSE_UPVALUE` 四指令、callable 收敛为闭包（`ObjFunction` 退为常量池内部物）、编译翻转 `resolve_upvalue` 递归捕获解析 + `CLOSE_UPVALUE` 作用域退出批量关闭；无新增 opcode，Disassembler 零改动）。
- **骨架待落地**：M5 类、M6 协程；CodeGen 对应特性占位 `NotImplemented`（编译期 Error），随 VM 里程碑逐个翻为真实发射。defer 善后机制已降级为可选后续（优先级最低，其他功能完成后另定，不绑定里程碑；try/finally 已裁撤的后继，见坑点文档「M3b finally 裁撤记录」）。
- 里程碑级细节见 `README.md` 与 `.claude/reference/runtime/vm-design.md` §6 路线表。

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
| `src/object/**` | `.claude/rules/object.md` | Object / ObjString / ObjFunction / ObjNativeFn / ObjModule / ObjException |
| `src/memory/**` | `.claude/rules/memory.md` | Buffer / Array / Allocator / HashTable / InternPool / GC |

### 深度设计文档 `.claude/reference/`（不自动加载，需要时 Read）

- `bytecode/bytecode-instruction-set.md` -- 指令集规格（功能 / 操作数位宽 / 栈效应 / 反汇编格式）。
- `runtime/vm-design.md` -- AriaVM / 执行上下文设计与 M1-M6 分阶段路线。
- `runtime/m4-closure-implementation-plan.md` -- M4 闭包实施计划（2026-09 定稿并已按四阶段全部落地：语义模型、三项设计决策与落地记录存档）。
- `runtime/m5-class-implementation-plan.md` -- M5 类实施计划（2026-09 定稿、待实施，M5 开工前重读：语义模型 + 六项设计决策——无 meta、静态与方法单表、构造期 bootstrap Object、bound-method 缓存进实例 fields 表（三铁则）、STORE_FIELD/MAKE_STATIC 镜像双指令、defining class 挂 ObjClosure）。
- `runtime/import-handling-overview.md` / `import-path-resolution.md` -- import 端到端处理与路径解析细节。
- `runtime/exception-implementation-pitfalls.md` -- M3 异常（try/catch/throw）踩坑归档（已落地；含 finally 裁撤记录与 defer 后继说明，及 M4 补录的 upvalue 关闭与 unwind 截栈/弹帧交互坑点，异常相关特性重启前重读）。
- `memory/gc-implementation-plan.md` -- GC 设计与 Phase 1/2 落地记录。
- `compile/compound-assignment-lowering.md` / `loopctx.md` -- 复合赋值 lowering、LoopCtx 与 break/continue 回填机制。

### 语言文法 `docs/grammar.txt`

手写文法规范。Lexer / Parser 的实现应与其保持一致，改文法时同步更新相关代码与测试。（注意「语言规范」与「解析器实现蓝图」两种视角可能存在差别，但描述的语法规则一致。）

### 软链接现状（单一事实源）

- 根目录 `AGENTS.md` **软链** → `CLAUDE.md`：ZCode 的工作区指令入口与本文件同一内容，修改只改本文件。
- `.zcode/skills/aria-<dir>/SKILL.md` **软链** → `../../../.claude/rules/<dir>.md`（9 个相对软链）：ZCode 把同一份规则文件暴露为技能，按 frontmatter `description` 自动触发；`paths:` 字段仅 Claude Code 消费。util/value/error/compile/bytecode/runtime/object/memory 与规则文件同名，`core.md` 对应技能 `aria-core`。
- 即两套 agent（Claude Code / ZCode）共享同一份本体文件，仓库内**无内容副本**。新增模块规则：建 `.claude/rules/<dir>.md`（frontmatter 带 name/description/paths），并补一条 `.zcode/skills/aria-<dir>/SKILL.md` 相对软链。

> 同步义务：改模块代码时同步更新对应 `.claude/rules/<dir>.md`；改设计时同步 `.claude/reference/` 对应文档；改文法时同步 `docs/grammar.txt` + 代码 + 测试。

## 构建

- CMake ≥ 3.20，C++23。clang++ / clang-format / clangd 均已在 PATH 中，可直接调用。在 `build` 目录中进行构建。
- 单文件语法检查必须带 `-I src`，否则 `common.hpp`/`type.hpp` 找不到：
  ```sh
  clang++ -std=c++23 -I src -fsyntax-only <file>
  ```
- 验证 TagValue 值表示（`common.hpp` 的 `USING_NANBOXING` 关闭路径）：独立 build 目录配 `-DARIA_USE_TAGVALUE=ON` 全量构建 + ctest：
  ```sh
  cmake -S . -B build/tagvalue -DARIA_USE_TAGVALUE=ON -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/tagvalue --target aria_tests -j
  ctest --test-dir build/tagvalue --output-on-failure
  ```
- 依赖 `third/isocline`（REPL）。IO 通过封装 `std::print`/`std::println` 实现。

## Git 提交纪律（强制）

- **改动完成不自行 commit**：代码/文档改完、验证全绿后即向用户报告并停手，改动留在工作区等 review；用户明确说提交（「commit」/「提交吧」等）后才执行 `git commit`。不抢先 `git add` 备提交。
- 用户确认提交后：一次阶段/里程碑一批，沿用仓库既有提交风格（`<模块>: <里程碑摘要> -- <分段细节>` 长行式，见 git log），提交说明覆盖改动动机、关键机制与验证结果。

## 输出与格式化（强制）

- **终端/日志输出统一用 `io::print` / `io::println`**（见 `util/io.hpp`，即对 `std::print`/`std::println` 的 using 别名），不要直接裸调 `std::print`/`std::cout`/`printf`。输出到 stderr 时用带 FILE* 的重载：`io::print(stderr, "...")`。
- **字符串格式化统一用 `std::format`**（`#include <format>`），不要混用 `sprintf`/`snprintf`/`fmt::format`/字符串拼接来构造格式化串。需要格式化结果字符串时用 `std::format("...", args...)`；需要直接输出时用 `io::println(std::format("...", args...))` 或 `io::print` 的格式串重载。
- **注释/文档破折号用 ASCII `--`（两个 hyphen），禁用 Unicode em-dash（U+2014）**：em-dash 被 Read 渲染成与 ASCII hyphen 难分的 `--`，会让 Edit 的 `old_string` 匹配失败。新增注释请保持用 ASCII `--`。

## 错误处理（src/error/）

原则常驻；Error 的静态工厂构造面 / 字段与尺寸 / ObjException 装箱载荷等结构细节见 `.claude/rules/error.md`（读 `src/error/**` 时自动加载）。

- **核心原则：内部用码，边界用 Error。** `ErrorCode`（1 字节纯码）供解释器**内部**判定（不变式断言 / 状态机分支 / 错误码到操作的映射），不关心位置与细节；`Error`（码 + `message_`，位置在构造期一次性烘焙为完整可读串，**不持 `SourceFile*`**，无悬空风险）是**边界与展示**的载体。注意 Error 的位置模型围绕编译期 `SourceLoc`（构造期烘 `path:line:col`），与运行期「装箱点由帧 `last_ip` 查行号表取位置」异位，故**运行期错误不就地构造 Error**，Error 仅在 `dispatch_loop()` 未捕获出口物化（见通道 2）。
- **四条错误通道**：
  1. **`Result<T, Error>` 返回（编译期通道 + VM 边界返回类型）**：编译期可恢复错误的常规通道（`Lexer::tokenize` / `Parser::parse` 恢复式收集、`Compiler::compile` 单错 Result）。VM 侧 `run()`/`interpret` 的 `Result` 仅为未捕获出口的边界返回类型，**不用于 dispatch_loop 内部逐站传播**（运行期在途错误走通道 2）。
  2. **VM 自管异常状态（运行期主通道，M3 已闭环）**：aria 的 throw/catch 与 VM 检测到的运行时错误统一走 VM 机制，**错误实体是 `ObjException`**（携 `ErrorCode` + 完整烘焙消息，位置前缀在装箱点由顶帧 `last_ip` 反推 offset 查行号表烘入）。装箱为 Value 存入**当前执行上下文的挂起错误寄存器**（`Movement::pending_error_`，随 `current_` 走、VM 根 tracer 标根）：装箱入口 `AriaVM::raise(code, detail)` 一步烘齐（复用 `Error::make_message` 烘焙单点，不经 Error 对象中转）；原生函数与 `call_value` 族以 bool 成败信号共用（惯用法 `return vm.fail(...)`）；dispatch_loop 内其余运行时错误站点经 `raise(code, fmt, ...)` 就地装箱（格式化归装箱入口）、用户 `throw` 经 `THROW` 弹值 `current_->raise(v)`（原值入寄存器不包，catch 绑原值保类型），raise 与 unwind 不融合，随后一律直接 `unwind()`（与 CALL 失败善后同形）。**派发与出口**：`AriaVM::unwind()` 自最内帧向外按帧 `last_ip` 反推 offset 查 CodeUnit 内**异常记录表**（`TryRecord{begin,end,handle,stack_depth}`，无 `SETUP_EXCEPT`/`END_EXCEPT` 操作码、不依赖 C++ 异常），命中即截值栈（`truncate_stack` 至 `slots + stack_depth`）跳 handler（异常值落 catch 参数槽），未命中逐帧 `exit_frame`；全未命中才物化 `Error`（匿名 `uncaught_error_parts` 反提拆 (码, 烘焙消息) 两件：ObjException 原码原消息、re-throw 保码；非 ObjException 兜底 `UncaughtException`，拼好跟踪后经 `Error::from_baked` 一次物化）并烘焙外->内逐帧 `at` 堆栈跟踪。落地状态见 `.claude/rules/runtime.md`「VM 异常通道（M3 已落地）」，设计见 vm-design.md §4.5-§4.8。
  3. **`AriaException` 派生**（C++ 异常）：仅用于 VM 之外、跨 C++ 调用栈的边界（Parser / CodeGen 深层 `fail()` 抛出、顶层 catch 翻译为 `Result`）；VM 主循环内不用（不跨 C++ 栈且是热路径）。
  4. **`fatal_error()`**（`[[noreturn]]`）：Internal / Resource 类不可恢复错误（`Unreachable`/`OutOfMemory`），打印 stderr 后 `std::exit(1)`。

## 工具

- **clang-format**（根目录 `.clang-format`，LLVM 风格 / 4 空格 / 120 列 / 命名空间全缩进）：`clang-format -i <file>` 原地格式化。编辑器保存时自动重排（如 `auto p`->`const auto p`）是项目风格，不要回退。
- **clangd**：读 `compile_commands.json`（CMake `EXPORT_COMPILE_COMMANDS` 生成）。注意 `compile_commands.json` 只含 `.cpp`/`.c`--header-only 头文件**不被任何编译 TU（直接或传递）include** 时会因拿不到编译参数报类型未定义假错；已被传递 include 的头 clangd 能推断参数（2026-09 实测 clangd 22：全部头文件 `clangd --check` 0 诊断假错，自含头即可）。疑似假错以 `clang++ -std=c++23 -I src -fsyntax-only` 实编译为准；根治：尽早让某 .cpp include 一次（仅对确实不可达的头需要）。

## 命名（强制）

只采用一套规则，不混 Google/LLVM/Unreal。优先级：可读性 > 一致性 > 简洁性。注释用中文。（完整规则见根目录 `CPP_Naming_Convention.md`，本节为其摘要。）

- 类型 / struct / enum class / 别名 / concept / 模板类型参数 / 枚举值：`PascalCase`
- 函数 / 变量 / 参数：`snake_case`
- 非静态成员：`snake_case_`（尾下划线）
- `constexpr`/`static const` 常量：`kPascalCase`
- 命名空间：`lowercase`；宏：`ALL_CAPS`
- 布尔用 `is_`/`has_`/`can_`/`should_`/`was_`/`needs_` 前缀
- 禁 Hungarian（`m_`）、禁保留标识符（`_Foo`）、禁宏风格常量（`MAX_SIZE`）

## 类型（src/type.hpp）

**不要直接用 `std::string`/`int`/`size_t` 等**，用别名：`i8..i64`/`u8..u64`/`isize`/`usize`/`f32`/`f64`/`String`/`StringView`/`List`/`HashMap`/`HashSet`/`Stack`/`Pair`/`Tuple`/`Span`/`UPtr`/`SPtr`/`Result<T,E>`（= `std::expected`）/`Opt<T>`。错误处理倾向 `Result` 返回而非抛异常。

## 代码组织

- util 层多为 header-only（`inline`，常 `constexpr`，无 `.cpp`）；新增头文件要同步加进 `CMakeLists.txt` 的 `aria_core` 源列表。
- 头文件守卫用 `#ifndef ARIA_<MODULE>_HPP`。
- 命名空间分层：`aria` 为根，子模块 `aria::fs`/`aria::utf8`/`aria::util`/`aria::io` 等；`source_file` 相关类型在 `aria::src` 下（`String`/`Result` 等在 `aria` 下，引用需分别 using）。
- 私有细节进 `detail` 子命名空间,且**按文件/模块隔离,勿共用顶层 `aria::detail`**(多文件共用会互相污染):有模块命名空间的放进 `aria::<module>::detail`(如 `aria::fs::detail`/`aria::utf8::detail`/`aria::util::detail`),没有的用 `aria::detail::<module>`(如 `aria::detail::ht`/`aria::detail::ast`);`.cpp` 内的文件局部细节用匿名命名空间(`namespace { }`)。平台分流用 `src/sys.hpp` 的 `SYS_WINDOWS`/`SYS_LINUX`/`SYS_MACOS`/`SYS_FREEBSD` 等宏；Windows 含 `<windows.h>` 前加 `WIN32_LEAN_AND_MEAN` 和 `NOMINMAX`。

## 类初始化

- **简单类**（纯数据聚合的小结构体，如 `src/source_file.hpp` 中的 `SourceSpan`/`LineCol`）：字段少、无逻辑、初始化无依赖，用类内默认成员初始化即可，不必上构造函数。
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

用 Google Test，位于 `tests/`（按 `tests/<module>/` 分目录）。GTest 通过 `FetchContent_Declare`（CMakeLists.txt 末尾）下载，配置时联网拉取 `v1.14.0`。

- 新增测试：在 `tests/<module>/` 加 `test_<module>.cpp`，并在 `tests/CMakeLists.txt` 的 `aria_tests` 源列表里登记。
- 测试链接 `aria_core` + `gtest_main`，用 `gtest_discover_tests` 注册到 ctest。
- 配置 + 构建 + 运行（构建目录用 `build/`，勿占用 IDE 的 `cmake-build-*`）：
  ```sh
  # Windows（clang，MinGW Makefiles 生成器）
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang
  # Linux / macOS（默认生成器，clang++ 或 g++）
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  cmake --build build --target aria_tests -j
  ctest --test-dir build --output-on-failure
  ```
- 临时文件用 `testing::TempDir()`（gtest 提供）写入，测试结束自动清理。

`build/` 与 `cmake-build-*` 均已加入 `.gitignore`。
