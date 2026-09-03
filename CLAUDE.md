# CLAUDE.md

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，打算支持 Windows / Linux / macOS）。当前进度：util 工具层（fs / utf8 / source_file / io / util / cli）与 value 层（NaN-boxing / TagValue 可切换）已就绪；error 层（ErrorCode / Error / AriaException / fatal_error）与 compile 层（Token / Lexer / AST / Parser）已实现；`OpCode` 枚举与 `FrameStack` 模板就绪；GC Phase 1 + Phase 2 已落地（详见 `.claude/reference/memory/gc-implementation-plan.md`）；`CodeUnit` 已落地（字节流 + 常量池 + RLE 行号表 + 异常记录表）；AriaVM M2 进行中（模块表 + 源根列表 + VM 根 tracer（标 `modules_` + `main_ctx_` 值栈/帧，**开发期即开 GC**：`run()`/`compile()` 不再持 `LockGuard`，`JUMP_BACK` 为 safe point；`compile()` 以 `make_guard(&module)` 根化建设中函数链，CodeGen 各 name 串跨子编译 `make_guard`）+ 全局指令 `DEF/LOAD/STORE_GLOBAL` + `IMPORT` 路径解析/模块表命中复用已落地，**模块磁盘加载/AST->CodeUnit 编译/run-once 已落地**（IMPORT 未命中分支:读盘 -> 编译(入口名 `<module>`) -> 入表 Loading -> 以 entry 作普通 0 参函数调用进帧交主循环执行;其 RETURN 按函数名 == `<module>` 判定模块体帧,弹弃返回值、置该模块 Loaded、改压模块对象,即「模块体返回模块」,命中/未命中栈效应统一；被导入模块编译期/运行期错误原样透传含其文件位置）），M3+（异常 try/catch、闭包、类、协程）仍为骨架；Object 子类型已落地 `ObjString`/`ObjFunction`/`ObjNativeFn`/`ObjModule`，其余未开始；`AstVisitor` 访问者基类已落地；字节码编译器 `CodeGen`（`AstVisitor` 具体子类，单遍合一：名字解析 + 语义检查 + 字节码发射）已落地--42 个 `visitXxxNode` 全 override，核心特性（算术/比较/逻辑短路/局部与全局/控制流/函数与递归/lambda/复合赋值/前置自增自减/import/for-in lowering）完整发射字节码，依赖未落地 VM 里程碑的特性（类/异常/闭包/list/map/field/index/match/range）占位 `NotImplemented`（编译期 Error），随 VM M3/M4/M5 推进逐个翻为真实发射。VM M1 主循环跑通、`Disassembler` 已落地。

> **模块参考与设计文档已拆出 CLAUDE.md**：各源码目录的模块参考在 `.claude/rules/`（带 `paths:` frontmatter，读对应源码时自动加载），设计文档在 `.claude/reference/`（按需 Read，不自动加载），语言文法 `grammar.txt` 留在 `docs/`。详见下「文档与参考」节。

## 构建

- CMake ≥ 3.20，C++23。clang++ / clang-format / clangd 均已在 PATH 中，可直接调用。在 `build` 目录中进行构建。
- 单文件语法检查必须带 `-I src`，否则 `common.hpp`/`type.hpp` 找不到：
  ```powershell
  clang++ -std=c++23 -I src -fsyntax-only <file>
  ```
- 依赖 `third/isocline`（REPL）。IO 通过封装 `std::print`/`std::println` 实现。

## 输出与格式化（强制）

- **终端/日志输出统一用 `io::print` / `io::println`**（见 `util/io.hpp`，即对 `std::print`/`std::println` 的 using 别名），不要直接裸调 `std::print`/`std::cout`/`printf`。输出到 stderr 时用带 FILE* 的重载：`io::print(stderr, "...")`。
- **字符串格式化统一用 `std::format`**（`#include <format>`），不要混用 `sprintf`/`snprintf`/`fmt::format`/字符串拼接来构造格式化串。需要格式化结果字符串时用 `std::format("...", args...)`；需要直接输出时用 `io::println(std::format("...", args...))` 或 `io::print` 的格式串重载。
- **注释/文档破折号用 ASCII `--`（两个 hyphen），禁用 Unicode em-dash（U+2014）**：em-dash 被 Read 渲染成与 ASCII hyphen 难分的 `--`，会让 Edit 的 `old_string` 匹配失败。新增注释请保持用 ASCII `--`。

## 错误处理（src/error/）

错误体系：`ErrorCode`/`ErrorCategory`（普通 `enum class : u8`，各 1 字节）、`Error`（仅 `ErrorCode code_` + `String message_` 两字段；**不持 `SourceFile*` 裸指针**--`message_` 构造期一次性烘焙为完整可读串 `"path:line:col: Category: Name detail"`（无位置则 `"Category: Name detail"`），构造后与 `SourceFile` 解耦、无悬空风险。`SourceLoc`=`SourceFile*`+`LineCol` 仍作 `Error` 构造参数类型与 `Token`/AST 节点的位置载体，`Error` 构造后不再持有它。**不保留结构化位置**：位置烘进 `message_` 即丢弃，解释器不做多错误按位置排序/去重）、`AriaException` 派生类（`AriaCompileException`/`AriaRuntimeException`，持 `Error`）、`fatal_error()`（`[[noreturn]]`，打印后 `std::exit`）。

- **核心原则：内部用码，边界用 Error。**
  - **`ErrorCode`（1 字节，纯码）** 用于解释器**内部**判定：不变式断言/不可恢复检查（`fatal_error(ErrorCode::Unreachable, ...)`）、分类与状态机分支（`switch (e.code())`、`e.code() == ErrorCode::X`）、不需要位置/消息的简单结果标志、错误码到操作的映射。即「只关心发生了什么类型的错，不关心在哪、细节」的场合。
  - **`Error`（带码+位置+源文件+消息）** 用于错误**出门**：跨阶段传递的最终载体（当前实例：`Lexer::tokenize() -> Result<List<Token>, List<Error>>`（词法错误恢复式收集）、`Parser::parse() -> Result<UPtr<ProgramNode>, List<Error>>`（语法错误 panic-mode 恢复式收集）；后续 `Result<CodeUnit, Error>` 等随编译器推进再加入）、异常构造（`throw AriaCompileException{Error{...}}`）、错误收集与打印（`List<Error>`、`e.message()`）。即词法/语法/语义阶段产出、要报给用户或传到别的阶段的错误。
- **四条错误通道**：
  1. **`Result<T, Error>` 返回**（项目默认风格，**已落地**）：可恢复错误的常规通道。当前实例：`Lexer::tokenize() -> Result<List<Token>, List<Error>>`（词法错误恢复式收集，见下）、`Parser::parse() -> Result<UPtr<ProgramNode>, List<Error>>`、`AriaVM::run() -> Result<Value, Error>`。VM `run_()` 主循环内部，操作码处理（如算术 `run_binary_numeric`、`CALL` 经 `call_value`）用 `Result<Value, Error>`/`Opt<Error>` 返回局部成败；成功时 `Error` 部分不构造，开销以 Result 类型尺寸为主。
  2. **VM 自管异常状态**（**设计目标，M1 部分落地**）：aria 语言的 `throw/catch` 与 VM 检测到的运行时错误（类型不符、越界等）**统一走 VM 自己的机制**--`op` 返回失败 `Error` 后，`run()` 调 `raise` -> 查 **CodeUnit 内异常记录表**定位最近覆盖当前 `ip` 的 `try` 记录，按记录登记的帧/栈深度 `truncate` 回退（unwind），跳到对应 handler。**不引入 `SETUP_EXCEPT`/`END_EXCEPT` 操作码**：`try` 块的范围与 handler 地址由编译期生成的**异常记录表**（存于 CodeUnit，每条记录 `try` 起始/结束 `ip` + `handler ip` + `catch` 参数槽等）登记，运行时按 `ip` 查表；比操作码方案更紧凑（不污染字节码流、无需每进/出 `try` 发指令）且便于反汇编。**不依赖 C++ 异常**（与 Lua/CPython 一致；动态类型语言运行时错误可能频繁，C++ 异常的栈展开代价不可控）。已就绪/尚未实现的清单见 `.claude/rules/runtime.md`「VM 异常通道落地状态」，设计见 `.claude/reference/runtime/vm-design.md` §4.7。
  3. **`AriaException` 派生**（C++ 异常，**已落地**）：仅用于 **VM 之外、跨 C++ 调用栈**的边界场景--编译期 parser 递归下降深处与 CodeGen 字节码编译器 visit 递归深处（`AriaCompileException`，Parser / CodeGen 均已实现：深层 `fail()` 抛出、顶层 catch 翻译为 `Result`）、REPL/嵌入 API 等顶层流程跨栈传播（`AriaRuntimeException`）。**VM `run()` 主循环内部不用 C++ 异常**（它不跨 C++ 栈、且是热路径）。
  4. **`fatal_error()`**（`[[noreturn]]`，**已落地**，定义于 `error/Error.hpp`）：不可恢复错误（Internal/Resource 类，如 `Unreachable`/`OutOfMemory`）打印到 stderr 后 `std::exit(1)`。
- **`AriaException` 与 aria 语言 `throw/catch` 无关**：前者是解释器 C++ 实现内部的错误传播；后者抛的是 aria Value，由 VM 用 `THROW` 操作码 + CodeUnit 内异常记录表实现（设计目标，不引入 `SETUP_EXCEPT`/`END_EXCEPT`，见上第 2 条）。不要混淆，也不要用 C++ 异常去实现 aria 语言的 throw。
- **`Error`（`String`（`message_`，构造期烘焙的完整可读串）+ `ErrorCode`，8 字节对齐；libstdc++ 下 40B、libc++ 下 32B；`message_` 构造期由 `SourceLoc` + 分类名 + 码名 + 细节一次拼成，含 `"path:line:col: "` 前缀或省略）**：报错是冷路径，成功时 `Result` 的 `Error` 部分不构造，故「大」主要影响 `Result` 类型尺寸而非热路径性能。**不保留结构化位置**：`Error` 不存 `SourceLoc`/`LineCol`，位置烘进 `message_` 即丢弃--解释器无需多错误按位置排序/去重，结构化位置只会徒增复杂度与生命期约束（「保留结构化位置供排序」属前期设计，已弃）。接口仅 `code()`/`message()`，无 `format()`/`location()`。

## 工具

- **clang-format**（根目录 `.clang-format`，LLVM 风格 / 4 空格 / 120 列 / 命名空间全缩进）：`clang-format -i <file>` 原地格式化。编辑器保存时自动重排（如 `auto p`->`const auto p`）是项目风格，不要回退。
- **clangd**：读 `compile_commands.json`（CMake `EXPORT_COMPILE_COMMANDS` 生成）。注意 `compile_commands.json` 只含 `.cpp`/`.c`--**未被任何 .cpp include 的 header-only 头文件（如 `utf8.hpp`/`source_file.hpp`）clangd 会因拿不到参数报类型未定义假错，以 `clang++ -std=c++23 -I src -fsyntax-only` 实编译为准。根治：尽早让某 .cpp include 一次。**

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

- **基础数据类型**（短类型名，见「类型」节的 `usize`/`i32`/`f64`/`bool` 等别名）优先 `T val = expr`/`T val{...}`，如 `const usize start = pos_;`--类型名本就短，显式写出比 `auto` 直白；与“类型极长”一行形成两端对照（短类型写类型名，长类型让位变量名）。
- **例外**：右侧是 `static_cast<T>`/`reinterpret_cast<T>` 等类型转换时，左侧统一用 `auto`（类型已在 cast 显式写明，左侧再写 `T` 重复），如 `const auto size = static_cast<usize>(end_pos);`。即使 `T` 是基础类型也用 `auto`，不套用“基础数据类型”那条。
- **构造用 `{}` 的例外**：大括号会触发 `initializer_list` 窄化或歧义时用小括号，如 `String(size, '\0')` 不能写 `String{size, '\0'}`（`size` 窄化为 `char` 报错），`auto result = String(size, '\0');` 是正确写法。
- 其余数据对象默认 `auto val = T{...}`（变量名优先）；行为/契约型对象（guard、公共 API）用 `T val{...}`（类型即语义）。
- `auto val = expr` 仅用于类型完全由右侧表达式决定的场合（工厂返回、`make_*`、推导），不要为省事对显式构造也用。

## 文档与参考

项目对 Claude 的上下文分三层组织（约定对齐 Claude Code `.claude/` 目录）：

- **`CLAUDE.md`（本文件）**：常驻上下文 -- 项目概览/进度 + 构建/输出/命名/类型/代码组织等**通用规则** + 错误处理原则 + 陷阱 + 测试。跨阶段通用、必须每次遵守的规则放这里。
- **`.claude/rules/`**：按源码目录拆分的**模块参考**，每个文件带 `paths:` frontmatter -- Claude 读到匹配路径的源码时**自动加载**对应参考，不读则不进上下文。当前 9 个：`util`/`value`/`error`/`compile`/`bytecode`/`runtime`/`object`/`memory`（各对应 `src/<dir>/**`）+ `core`（对应 `src/common.hpp`/`type.hpp`/`sys.hpp`/`main.cpp`/`interpreter.hpp` 等顶层文件）。改某模块代码时其参考自动出现，无需手动翻 CLAUDE.md。
- **`.claude/reference/`**：深度**设计文档**（按主题分类，不自动加载，Claude 需要时按需 Read）：`bytecode/bytecode-instruction-set.md`（指令集：功能/操作数位宽/栈效应）、`memory/gc-implementation-plan.md`（GC 实现计划，Phase 1/2 落地细节）、`runtime/vm-design.md`（AriaVM/执行上下文设计与分阶段路线）、`runtime/import-handling-overview.md`（import 端到端处理概览）、`runtime/import-path-resolution.md`（IMPORT 路径解析细节）、`runtime/exception-implementation-pitfalls.md`（M3 异常 try/catch/throw 实现踩坑归档，下次实现前必读）、`compile/compound-assignment-lowering.md`（复合赋值 lowering 设计）、`compile/loopctx.md`（LoopCtx 结构体与 break/continue 占位回填机制）。
- **`docs/grammar.txt`**：语言文法（手写规范，留在 `docs/`）。Lexer / Parser 的实现应与 `grammar.txt` 保持一致，改文法时同步更新相关代码与测试。（注意**文法作为「语言规范（Specification）」** 与 **文法作为「解析器实现蓝图（Parser Blueprint）」** 可能存在区别，但两者实际描述的语法规则是一样的。）

> 改模块代码时同步更新对应 `.claude/rules/<dir>.md`；改设计时同步更新对应 `.claude/reference/...`；改文法时同步更新 `docs/grammar.txt` + 代码 + 测试。

## 关键模块

各源码目录的模块参考已按目录拆分到 `.claude/rules/`（带 `paths:` frontmatter，读对应源码时自动加载）：

| 源码目录 | 参考文件 | 覆盖 |
| --- | --- | --- |
| `src/common.hpp`/`type.hpp`/`sys.hpp`/`main.cpp`/`interpreter.hpp` | `.claude/rules/core.md` | common(宏/USING_NANBOXING) / type(别名) / sys(平台宏) / main(解释器入口) / interpreter(CLI 分发核心) |
| `src/util/**` | `.claude/rules/util.md` | fs / utf8 / source_file / io / util / cli |
| `src/value/**` | `.claude/rules/value.md` | Value(NanBoxing/TagValue) / AriaArray / AriaHashTable |
| `src/error/**` | `.claude/rules/error.md` | ErrorCode / Error / AriaException |
| `src/compile/**` | `.claude/rules/compile.md` | Token / Lexer / ast / Parser / AstVisitor |
| `src/bytecode/**` | `.claude/rules/bytecode.md` | code.hpp / CodeUnit / Disassembler |
| `src/runtime/**` | `.claude/rules/runtime.md` | FrameStack / Movement / AriaVM（含异常通道落地状态） |
| `src/object/**` | `.claude/rules/object.md` | Object / ObjString / ObjFunction / ObjNativeFn / ObjModule |
| `src/memory/**` | `.claude/rules/memory.md` | Array / Allocator / HashTable / InternPool / GC |

深度设计文档见 `.claude/reference/`（按需 Read，见上「文档与参考」节）。

## 陷阱

- `SourceFile` 的 `content()`/`name()`/`path()` 返回 `StringView`，不得比 `SourceFile` 活得更久；多 `SourceFile` 存容器并已取 `StringView` 后勿再增删致重分配（SSO 改地址）。
- **`SourceFile` 以指针传入（非拥有）**：`Lexer::tokenize(SourceFile*)` 直接接 `SourceFile*`；`Token` 经 `SourceLoc`（`SourceFile* src` + `LineCol`）持源文件指针（`SourceLoc` 显式构造断言 src 非空、默认构造为空态 src=nullptr），`Token::loc_` 为 `SourceLoc`（空态表无位置）。**`Error` 不在此列**--`Error` 构造期已把 `SourceLoc` 烘进自有 `message_` 串，不再持 `SourceFile*`，故与 `SourceFile` 生命周期解耦。**调用方须保证 `SourceFile` 在所有借出的 `StringView`（`Token::lexeme`）与 `Token::loc_` 内 `SourceLoc::src_` 使用期间存活且地址不变**--尤其注意：`SourceFile` 含 `String content_`，**SSO 短串（短于阈值，如 `"_"`/`"f"`）move 后 data 地址会变**（SSO buffer 跟随对象，move 是逐字节拷贝）。故 token 流持有的指向 `content_` 的 view 与 `Token::loc_` 内 `SourceLoc::src_`，其 `SourceFile` 不得在它们存活期被 move。实践中：让 `SourceFile` 就位后再 tokenize，之后不再 move 该对象（如放进 `unique_ptr` 容器或长寿命成员）。`Error` 因已自有位置串，无此约束。
- 源码加载时 CRLF/CR 已归一化为 LF，`line`/`locate` 内部只按 `\n` 切行。

## 测试

用 Google Test，位于 `tests/`。GTest 通过 `FetchContent_Declare`（CMakeLists.txt 末尾）下载，配置时联网拉取 `v1.14.0`。

- 新增测试：在 `tests/` 加 `test_<module>.cpp`，并在 `tests/CMakeLists.txt` 的 `aria_tests` 源列表里登记。
- 测试链接 `aria_core` + `gtest_main`，用 `gtest_discover_tests` 注册到 ctest。
- 配置 + 构建 + 运行（构建目录用 `build/`，勿占用 IDE 的 `cmake-build-*`）：
  ```powershell
  # Windows（clang，MinGW Makefiles 生成器）
  cmake -S . -B build -G "MinGW Makefiles" `
        -DCMAKE_CXX_COMPILER=clang++ `
        -DCMAKE_C_COMPILER=clang
  ```
  ```sh
  # Linux / macOS（默认生成器，clang++ 或 g++）
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
  ```
  ```powershell
  cmake --build build --target aria_tests -j
  ctest --test-dir build --output-on-failure
  ```
- 临时文件用 `testing::TempDir()`（gtest 提供）写入，测试结束自动清理。
- 语法快速检查仍可用 `clang++ -std=c++23 -I src -fsyntax-only`。

`build/` 与 `cmake-build-*` 均已加入 `.gitignore`。
