---
name: aria-testing
description: aria 测试组织与新增测试流程：写 C++ GTest 用例或 aria 语言语料时使用。含 tests/<module>/ 布局与 CMake 登记、语料判定规则（assert 收口 / .out 逐字节 / .err 子串）、写作规范与当前禁区、ctest 用法与双配置验证。
---

# aria 测试指南

> 场景：为某模块补 C++ 白盒测试、写 aria 语言语料、或跑/登记测试。组织总览见
> `docs/testing.md`；语料细则见 `tests/language/README.md`；GTest 位于 `tests/<module>/`、
> 链接 `aria_core` + `gtest_main`。

## 1. 两层测试

| 层 | 位置 | 量什么 |
| :--- | :--- | :--- |
| C++ 白盒 | `tests/<module>/`（util / value / compile / bytecode / runtime / object / memory） | 模块内部行为，直接调 C++ API |
| 语言语料 | `tests/language/`（唯一 C++ 文件是参数化 runner） | 端到端语言行为，兼 GC 压力安全网（每 VM 开 stress GC） |

## 2. 新增 C++ 测试

1. 建 `tests/<module>/test_<模块>.cpp`，**登记进 `tests/CMakeLists.txt` 的 `aria_tests`
   源列表**（漏登记 = 不编译不运行）。
2. 临时文件用 `testing::TempDir()`，结束自动清理。
3. `gtest_discover_tests` 自动注册 ctest，重新构建后即收。

## 3. 新增语言语料（细则见 `tests/language/README.md`）

- 选层：正向放 `positive/<层>/`（01_lexical ... 14_coroutines）；负向按阶段放
  `negative/compile_errors/`（`compile_` 前缀）或 `negative/runtime_errors/`
  （`runtime_` 前缀）。文件名 snake_case 表意，单脚本建议 < 80 行。
- 判定：正向须 `Ok` 且脚本内 `assert` 自检；配同名 `.out` 则 stdout 逐字节比对；
  负向须 `CompileError` / `RuntimeError`；配 `.err` 则 stderr 每行一个子串全部出现。
- 模块用例（09_modules）每用例一目录：`用例名/main.aria` + `用例名/lib/*.aria`
  （`lib` 被 runner 跳过）。
- **禁区**：不钉临时未实装行为（见该 README「当前禁区」节，翻转日会变红）；
  `.err` 禁绝对路径与漂移行号；`.out` golden 从当前构建实测生成、人工核对后冻结。

## 4. 跑法

```sh
cmake --build build --target aria_tests -j
ctest --test-dir build --output-on-failure          # 全量
ctest --test-dir build -R <用例名片段>              # 单点；语料 PRE_TEST 发现，新增脚本免重链接
```

- **先确认 build 退出码再跑测试**：构建失败时 ctest 可能拿旧二进制假绿。
- **双配置验证**：改语言面 / 值表示相关代码后，另配 `build/tagvalue`
  （`-DARIA_USE_TAGVALUE=ON`）全量重跑；纯注释与文档改动不必。

## 5. 基准不属于测试

`bench/` 是性能语料（逐文件起进程计时、跨语言对照），验收纪律见技能 `aria-bench`；
测试判对错，基准量快慢，别把基准跑绿当测试通过。
