---
name: aria-testing
description: 写 aria 的 C++ GTest 用例或语言语料时使用。核对 CMake 登记、语料位置、期望文件与双配置验收；运行命令见 docs/testing.md，语料判定规则见 tests/language/README.md。
---

# aria 测试指南

> 组织与跑法见 [测试文档](../../../docs/testing.md)，判定与命名规则见 [语料 README](../../../tests/language/README.md)。

## 1. 新增 C++ 用例

- 新文件按模块放置，确认登记进 `tests/CMakeLists.txt` 的 `aria_tests` 源列表。漏登记不会参与编译。
- 用例写临时文件时核对并行运行是否会冲突，按[测试文档](../../../docs/testing.md#编写测试)处理。

## 2. 新增语言语料

- 按[语料 README](../../../tests/language/README.md#目录结构与判定规则)核对脚本所在目录、文件名前缀和旁挂文件。挂错分支的期望文件不会生效，拼错正向层目录名也不会报错。
- 用例须有可观察的判据。要验证栈平衡时，安排后续局部读取或调用暴露槽位偏移；只通过 stress GC 不足以证明栈平衡。
- 按[新增语料跑法](../../../docs/testing.md#验证新增语言脚本)直接运行 GTest，确认新用例实际执行。

## 3. 验收

- 确认当前版本构建成功，再记录测试结果。
- 改语言行为或值表示后，按[双配置要求](../../../docs/testing.md#值的两种表示)验证两份构建。
