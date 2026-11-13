# 测试

测试分两层，各验证不同的东西：

- **C++ 单元测试**（`tests/<module>/`，按源码模块分目录）：直接调用解释器内部的 C++
  接口，验证各个模块自身的行为。
- **语言测试脚本**（`tests/language/`）：用 aria 语言写成的完整程序，端到端运行解释器，
  验证语言的实际行为。这些脚本运行时会把 GC 设为压力模式，因此同时也在检验垃圾回收的
  正确性。

## 运行测试

```sh
cmake --build build --target aria_tests -j && ctest --test-dir build --output-on-failure --no-tests=error
```

只运行已登记、名字匹配的用例：

```sh
ctest --test-dir build -R '用例名片段' --output-on-failure --no-tests=error
```

如果编译失败，先修复编译错误再运行 ctest：测试程序是上一次成功构建的产物，直接运行
得到的是旧版本的结果。

### 验证新增语言脚本

已有测试二进制时，不必重新构建。直接运行 GTest，它每次启动都会扫描语料目录：

```sh
build/tests/aria_tests --gtest_filter='*用例名片段*'
```

Windows 的可执行文件为 `build/tests/aria_tests.exe`（Ninja 单配置构建）。检查输出是否实际跑了
目标用例：GTest 匹配不到用例时也可以返回成功。语料用例名由相对路径去掉 `.aria`、把 `/`
换成 `_` 得到，模块用例可用目录名匹配。

CTest 的 `PRE_TEST` 模式会复用已有发现清单，单独增删脚本不会让它失效。语料清单改动后，
全量验收可在新构建目录运行 CTest，或直接运行不带筛选的 GTest。CTest 的
`--no-tests=error` 只能防止完全没匹配到测试，不能发现清单里缺了部分用例。

## 编写测试

**C++ 测试**放在 `tests/<对应模块>/` 下，文件名按主题取（如 `test_gc.cpp`、
`test_array.cpp`，一个模块可以有多个文件），并添加到 `tests/CMakeLists.txt` 的
`aria_tests` 源列表——没有登记的文件不会参与编译，也就不会运行。测试需要临时文件时，
`testing::TempDir()` 只返回系统临时目录，不会建目录也不会清理：文件名要带用例名（并行
运行 `ctest -j` 时共享同一目录），需要的话在用例里自己建子目录并删除。

**语言测试脚本**的目录结构、判定方式和期望文件写法见 [语料 README](../tests/language/README.md)。

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

推送到 `main` 或向 `main` 提交 PR 时，CI 运行文档、报错文案与代码格式检查，并在
Linux、Windows、macOS 构建和测试，其中包含 Linux TagValue 配置与三平台 Release 配置。
