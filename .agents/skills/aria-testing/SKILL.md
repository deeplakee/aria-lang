---
name: aria-testing
description: aria 测试组织与新增测试流程：写 C++ GTest 用例或 aria 语言语料时使用。含 tests/<module>/ 布局与 CMake 登记、语料判定规则（assert 收口 / .out 逐字节 / .err 子串）与放错位置的静默失效、ctest 用法与双配置验证。组织总览见 docs/testing.md。
---

# aria 测试指南

> 场景：为某模块补 C++ 白盒测试、写 aria 语言语料、跑/登记测试。组织总览与构建选项见 `docs/testing.md`，
> 语料细则见 `tests/language/README.md`（判定规则本体在那里，这里只留判据与坑）。

## 1. 两层测试

| 层 | 位置 | 量什么 |
| :--- | :--- | :--- |
| C++ 白盒 | `tests/<module>/` | 模块内部行为，直接调 C++ API |
| 语言语料 | `tests/language/`（唯一 C++ 文件是参数化 runner） | 端到端语言行为，兼 GC 压力安全网（每 VM 开 stress GC） |

## 2. 新增 C++ 用例

1. 文件按**主题**命名放模块目录下（`tests/memory/test_gc.cpp`、`test_array.cpp`、`test_ast_arena.cpp`…），不要求「一模块一文件」。
2. **登记进 `tests/CMakeLists.txt` 的 `aria_tests` 源列表**--漏登记 = 不编译不运行，且没有任何提示。
3. `gtest_discover_tests` 自动注册 ctest，重新构建即收。

**临时文件**：`testing::TempDir()` 只返回**系统临时目录**，**不会**建目录、也不自动清理（gtest 文档原话 "Returns a path to a temporary directory, which should be writable"）。它是全机共享的，`ctest -j` 并行时用固定文件名会互相覆盖--要么文件名带用例名，要么在用例内自建子目录并自己 `remove`。

## 3. 新增语言语料（判定规则本体见 `tests/language/README.md`）

- 选层：正向 `positive/<层>/`；负向按阶段放 `negative/compile_errors/`（`compile_` 前缀）或 `negative/runtime_errors/`（`runtime_` 前缀）。文件名 snake_case 表意。
- 判定：正向须 `Ok` 且脚本内 `assert` 自检；配同名 `.out` 则 stdout **逐字节**比对（含末尾换行）。负向须 `CompileError` / `RuntimeError`；配 `.err` 则 stderr 每行一个子串、全部出现即可。
- 模块用例（09_modules）每用例一目录：`用例名/main.aria` + `用例名/lib/*.aria`（`lib` 被 runner 跳过）。

**三个静默失效点**：

- `.out` 只在正向分支读、`.err` 只在负向分支读。**挂错位置的判定文件不报错也不生效**。
- runner 递归收集全部 `.aria`，**层目录名写错不会报错**，只会静静成为新层；层清单要人工改两处（`tests/language/README.md` 与本节）。
- 语料跑绿不证明**栈平衡**：stress GC 全绿只说明根接线对，栈形失衡要另写断言（或看后续行为是否错乱）。

**禁区**：不钉未实装行为（见该 README「当前禁区」节）；`.err` 禁绝对路径与漂移行号；`.out` golden 从当前构建实测生成、人工核对后冻结。

## 4. 跑法

```sh
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure          # 全量
ctest --test-dir build -R <后缀片段>                 # 单点
```

- **先确认 build 退出码再跑测试**：构建失败时 ctest 拿旧二进制照样绿。
- `-R` 的用例名带套件前缀，语料的实际形态是 `LanguageCorpus/LanguageCorpusSuite.MatchesExpectedOutcome/positive_01_lexical_comments_line_forms`（相对路径的 `/` 换成 `_`），按后缀片段匹配。
- **双配置验证**：改语言面或值表示相关代码后另配 `build/tagvalue`（`-DARIA_USE_TAGVALUE=ON`）全量重跑；纯注释与文档改动不必。

## 5. 基准不属于测试

`bench/` 分两层：`lexer_bench.cpp` / `vm_bench.cpp` / `hashtable_bench.cpp` 是进程内微基准，`bench/lang/` 才是逐文件起进程 + 跨语言对照。纪律见技能 `aria-bench`；测试判对错，基准量快慢，别把基准跑绿当测试通过。
