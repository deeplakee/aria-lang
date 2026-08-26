# aria

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，目标支持 Windows / Linux / macOS）：栈式字节码 VM + 单遍合一的字节码编译器 + mark-sweep GC，开发期即开 GC。

## 当前状态

- **已落地**：util 工具层（fs / utf8 / source_file / io）、value 层（NaN-boxing / TagValue 可切换）、error 层、compile 层（Token / Lexer / AST / Parser / AstVisitor）、字节码层（OpCode / CodeUnit / Disassembler）、字节码编译器 CodeGen（42 个 `visitXxxNode` 全 override）、GC Phase 1 + Phase 2、Object 子类型 ObjString / ObjFunction / ObjNativeFn / ObjModule。
- **进行中**：AriaVM M2（模块表 + 源根列表 + VM 根 tracer + 全局 `DEF/LOAD/STORE_GLOBAL` + `IMPORT` 路径解析/命中复用；磁盘加载 / 被导入模块编译 / run-once 待续）。
- **待续**：M3 异常 try/catch、M4 闭包、M5 类、M6 协程。

## 构建

CMake ≥ 3.20，C++23，clang++ / clang-format / clangd 均需在 PATH 中。

```sh
# Windows（MinGW Makefiles + clang）
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang

# Linux / macOS（默认生成器）
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
```

单文件语法检查（必须带 `-I src`，否则 `common.hpp`/`type.hpp` 找不到）：

```sh
clang++ -std=c++23 -I src -fsyntax-only <file>
```

## 测试

Google Test（CMake `FetchContent` 拉 v1.14.0，配置时联网）。

```sh
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure
```

## 项目结构

```
src/
  util/      fs / utf8 / source_file / io / util 工具（多 header-only）
  value/     Value（NanBoxing / TagValue）/ AriaArray / AriaHashTable
  error/     ErrorCode / Error / AriaException
  compile/   Token / Lexer / ast / Parser / AstVisitor / FunctionCtx / ModuleCtx / CodeGen
  bytecode/  OpCode / CodeUnit / Disassembler
  object/    Object / ObjString / ObjFunction / ObjNativeFn / ObjModule
  memory/    Buffer / Array / Allocator / HashTable / InternPool / GC
  runtime/   FrameStack / Movement / AriaVM
tests/      Google Test 单测
bench/      性能基准（独立可执行）
third/      isocline（REPL）
docs/       grammar.txt（语言文法规范）
```

## 进一步阅读

- `CLAUDE.md` -- 项目规则、进度、构建 / 命名 / 类型 / 错误处理等通用约定（常驻上下文）。
- `CPP_Naming_Convention.md` -- C++ 命名规范。
- `.claude/rules/` -- 按源码目录拆分的模块参考（带 `paths:` frontmatter，读对应源码时自动加载）。
- `.claude/reference/` -- 深度设计文档（GC 计划、VM 设计、指令集、import 处理、lowering 等，按需阅读）。
- `docs/grammar.txt` -- 语言文法。