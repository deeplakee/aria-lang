# 测试

测试分两层，各验证不同的东西：

- **C++ 单元测试**（`tests/<module>/`，按源码模块分目录）：直接调用解释器内部的 C++
  接口，验证各个模块自身的行为。
- **语言测试脚本**（`tests/language/`）：用 aria 语言写成的完整程序，端到端运行解释器，
  验证语言的实际行为。这些脚本运行时会把 GC 设为压力模式，因此同时也在检验垃圾回收的
  正确性。

## 运行测试

```sh
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure    # 运行全部测试
ctest --test-dir build -R <名字片段>           # 只运行名字匹配的测试
```

如果编译失败，先修复编译错误再运行 ctest：测试程序是上一次成功构建的产物，直接运行
得到的是旧版本的结果。

## 编写测试

**C++ 测试**放在 `tests/<对应模块>/` 下，文件名按主题取（如 `test_gc.cpp`、
`test_array.cpp`，一个模块可以有多个文件），并添加到 `tests/CMakeLists.txt` 的
`aria_tests` 源列表——没有登记的文件不会参与编译，也就不会运行。测试需要临时文件时，
`testing::TempDir()` 只返回系统临时目录，不会建目录也不会清理：文件名要带用例名（并行
运行 `ctest -j` 时共享同一目录），需要的话在用例里自己建子目录并删除。

**语言测试脚本**的完整规则（目录结构、判定方式、期望输出文件的写法）见
`tests/language/README.md`。要点：正向脚本必须成功运行，并用 `assert` 验证结果；反向
脚本要求程序在编译期或运行期失败。新增脚本会被 ctest 自动发现，不需要重新构建。

## 值的两种表示

解释器的值默认用 NaN-boxing 表示，也可以切换为 TagValue（构建选项
`-DARIA_USE_TAGVALUE=ON`，见 [building.md](building.md)）。两种表示应当行为一致：改动
值表示或语言行为相关的代码后，在单独的构建目录再做一次 TagValue 构建并运行全部测试，
两种配置都通过才算完成。

## 性能基准

`bench/` 与测试相互独立，它测量快慢而不是判定对错：

- `lexer_bench` / `vm_bench` / `hashtable_bench`：进程内微基准，测量词法分析、字节码
  派发、哈希表等底层操作的开销。
- `bench/lang/`：56 个完整的 aria 程序，与 Python / JavaScript / Lua / Java 的同算法
  移植逐个对比运行时间。清单与驱动用法见 `bench/lang/README.md`。
- `bench/profile/`：统计真实负载中各类指令的执行频度，为优化方向提供依据，用法见其
  目录下的 README。

测量一律使用 Release 构建（见 [building.md](building.md)）。

## 持续集成

`.github/workflows/ci.yml` 在每次提交时运行：报错文案与代码格式检查；Linux、Windows、
macOS 三平台的构建与全部测试，其中包含 TagValue 配置与 Release 配置。
