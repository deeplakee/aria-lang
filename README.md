# aria

aria 是用 C++23 实现的**跨平台**解释器（自研脚本语言，目标支持 Windows / Linux / macOS）：栈式字节码 VM + 单遍合一的字节码编译器 + mark-sweep GC，开发期即开 GC。

## 当前状态

- **已落地**：util 工具层（fs / utf8 / source_file / io / util / cli）、value 层（NaN-boxing / TagValue 可切换）、error 层、compile 层（Token / Lexer / AST / Parser / AstVisitor / CodeGen / Compiler）、字节码层（OpCode / CodeUnit / Disassembler）、字节码编译器 CodeGen（43 个 `visitXxxNode` 全 override）、GC Phase 1 + Phase 2、Object 子类型 ObjString / ObjFunction / ObjNativeFn / ObjModule / ObjException / ObjClosure / ObjUpvalue / ObjClass / ObjInstance / ObjBoundMethod、AriaVM M1 主循环 + M2（模块表 / 源根 / 全局 `DEF/LOAD/STORE_GLOBAL` / builtins type·len·str·assert / `IMPORT` 磁盘加载全链 / 运行期报错位置标注）+ M3 异常 try/catch/throw（统一寄存器通道 + unwind 查异常记录表、跨帧捕获、re-throw 保码、未捕获堆栈跟踪）+ M4 闭包（「捕获即引用」语义、open upvalue 开链、`CLOSURE/LOAD_UPVALUE/STORE_UPVALUE/CLOSE_UPVALUE` 四指令、callable 收敛为闭包）+ M5 类（静态+方法单表、读穿透/写遮蔽、bound 缓存、`MAKE_CLASS/MAKE_METHOD/MAKE_STATIC/LOAD_SUPER_FIELD` 等九指令、编译翻转：def lowering / 字段访问与赋值 / this 槽 0 捕获 / super 链读）。
- **待续**：P0 语言面补齐里程碑（计划见 `.claude/reference/runtime/collections-builtin-methods-plan.md`：批 1-5 已落地——值寄存器组底座 + 默认参数、match 降糖、list 值表示与下标、内置类型方法机制（bootstrap 类）+ 迭代协议（每源迭代器子类）+ varargs、map 字面量/下标与迭代产出 `[k, v]`；待批 6+——string 方法、ObjRange、解构、INVOKE_METHOD 性能批），其后 M6 协程；defer 善后为可选后续（优先级最低，其他功能完成后另定；try/finally 已裁撤）。M4 闭包实施计划与落地记录见 `.claude/reference/runtime/m4-closure-implementation-plan.md`，M5 类实施计划见 `.claude/reference/runtime/m5-class-implementation-plan.md`。

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
  main.cpp / interpreter.hpp / interpreter.cpp   CLI 入口与分发
  util/      fs / utf8 / source_file / io / util / cli 工具（多 header-only）
  value/     Value（NanBoxing / TagValue）/ AriaArray / AriaHashTable
  error/     ErrorCode / Error / AriaException
  compile/   Token / Lexer / ast / Parser / AstVisitor / FunctionCtx / ModuleCtx / CodeGen / Compiler
  bytecode/  code.hpp（OpCode）/ CodeUnit / Disassembler
  object/    Object / ObjString / ObjFunction / ObjNativeFn / ObjModule / ObjException
  memory/    Buffer / Array / Allocator / HashTable / InternPool / GC
  runtime/   FrameStack / Movement / AriaVM / Builtins
tests/      Google Test 单测（按 tests/<module>/ 分目录）
bench/      性能基准（独立可执行）
external/   isocline（REPL）
docs/       grammar.txt（语言文法规范）
```

## 进一步阅读

- `CLAUDE.md`（根目录 `AGENTS.md` 为其软链，ZCode 指令入口同源）-- 项目规则、进度、构建 / 命名 / 类型 / 错误处理等通用约定（常驻上下文）。
- `CPP_Naming_Convention.md` -- C++ 命名规范。
- `.claude/rules/` -- 按源码目录拆分的模块参考（带 `paths:` frontmatter，读对应源码时自动加载）。
- `.claude/reference/` -- 深度设计文档（GC 计划、VM 设计、指令集、import 处理、lowering 等，按需阅读）。
- `docs/grammar.txt` -- 语言文法。