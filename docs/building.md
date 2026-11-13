# 构建与运行

aria 使用 CMake 构建，需要 CMake 3.20 或更高版本，以及支持 C++23 的编译器。
REPL 依赖的 isocline、测试框架 Google Test、内存分配器 mimalloc 都已随仓库放在
`external/` 下，配置和构建全程不需要联网。

## 配置与构建

Linux 与 macOS：

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target aria -j
```

Windows 推荐 Ninja 加 clang 工具链（当 clang++ 以 MSVC 为目标时，链接由本机的
Visual Studio 完成）：

```sh
cmake -S . -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target aria -j
```

构建产物 `build/aria` 是解释器的命令行程序：

```sh
build/aria script.aria          # 运行脚本文件
build/aria -e 'println(1 + 1);' # 执行一段源码
build/aria --repl               # 进入交互式环境
build/aria --version
```

交互式环境中，顶层 `var` 声明的变量在后续输入里保持可用，适合用来试验语言行为。

## 构建类型与选项

日常开发使用 Debug 即可；性能测量必须使用 Release——它默认开启 `-O3` 与链接时优化
（LTO），基准读数都以这一配置为准，Debug 与 Release 的差距可以达到一个数量级。需要
临时关闭 LTO 时，配置时加 `-DARIA_ENABLE_LTO=OFF`。

其余 CMake 选项（默认值以 `CMakeLists.txt` 为准）：

| 选项 | 默认 | 作用 |
| --- | --- | --- |
| `ARIA_USE_TAGVALUE` | OFF | 用 TagValue 表示值，替代默认的 NaN-boxing。两种表示语义等价；切换后应在新的构建目录重新构建并运行全部测试。 |
| `ARIA_USE_MIMALLOC` | ON | 内存分配器使用 mimalloc（以普通库的方式链接，不接管进程的 malloc）；关闭后退回标准库分配器。 |
| `ARIA_ENABLE_LTO` | ON | Release 构建开启链接时优化。 |
| `ARIA_DEBUG_GC` | OFF | 打开 GC 的调试日志。 |
| `ARIA_DEBUG_PRINT_CODE` | OFF | 运行前打印每个函数编译后的指令序列。 |
| `ARIA_DEBUG_TRACE_EXECUTION` | OFF | 逐条指令打印执行轨迹。 |
| `ARIA_OPCODE_PROFILE` | OFF | 统计指令执行频度，配合 `bench/profile/` 使用，用法见其 README。 |

不同用途的构建放在不同目录互不干扰比较省心，例如 `build/` 做日常开发、`build/rel`
做性能测量；`build` 及其各种变体目录都已被 git 忽略。

## 开发环境

- **代码格式化**使用根目录的 `.clang-format`（LLVM 风格、4 空格缩进、120 列）。CI 用
  clang-format 23.1.3 检查纳入版本管理的 `.cpp` 与 `.hpp` 文件，本地格式化请使用同一
  版本：`clang-format -i <file>`。
- **clangd** 直接可用：CMake 配置时默认生成 `compile_commands.json`。个别没有被任何
  `.cpp` 包含的头文件可能因缺少编译参数而报错，以实际编译结果为准。
- 快速检查单个源文件的语法：`clang++ -std=c++23 -I src -fsyntax-only <file>`，其中
  `-I src` 是为了让 `src/` 下的公共头文件可见。
