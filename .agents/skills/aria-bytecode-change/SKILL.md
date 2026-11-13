---
name: aria-bytecode-change
description: 加或改一条字节码指令（OpCode）时使用。列出完整同步面：code.hpp X 表追加与枚举序、dispatch_loop 的 case（漏 case 是运行期 fatal）、opcode_profile 的平行 switch 与 size_of/records_fall_pair、反汇编渲染与 kOpCodeCount 哨兵、CodeGen/CodeUnit 发射侧、instruction-set.md 与 lowering.md、语料与画像。
---

# 加 / 改一条指令

> 场景：新增 OpCode、改某条指令的栈效应或操作数格式。见[指令规格](../../reference/specs/instruction-set.md)
> 与 [CodeUnit 容器和发射接口](../../reference/bytecode.md)。

## 1. 定形

先在 `instruction-set.md` 里想清楚三件事：**操作数格式**（选既有 `OpFormat` 就少改两处）、**栈效应**（右侧为栈顶；`STORE_*` 必须 peek-store、留被存值）、**谁发射**（CodeGen 哪个 visit 方法，还是只给手写字节码用）。

## 2. 同步面（按序过）

1. **`src/bytecode/code.hpp` 的 `ARIA_OPCODE_LIST`**：**末尾追加**一行 `X(NAME, Format)`。枚举顺序即 opcode 数值，插中间会移动所有后续指令的编号（`kOpCodeCount`、画像输出、手写字节码全受影响）。
2. **`tests/bytecode/test_disassembler.cpp` 的 `EXPECT_EQ(kOpCodeCount, 79u)`**：X 表行数哨兵，加一条就改这个数（不改则测试红，这是它存在的意义）。
3. **`src/runtime/Eval.cpp` 的 `dispatch_loop`**：加 `case`。漏了不会编译错--落 `default: UNREACHABLE()`，**运行期 fatal**。
4. **`src/runtime/opcode_profile.cpp`**：同步 `fetch` 的全枚举 switch。新增格式时核对 `size_of`（默认按 3 字节）；改变出边时核对 `records_fall_pair` 与 `is_frame_switch`（分别按格式和指令分类）。
5. **`Disassembler`**：按格式表自动分发，既有格式零改动；**只有新格式**才加分发分支。渲染回归在 `tests/bytecode/test_disassembler.cpp`。
6. **发射侧**：`CodeGen` 的 visit 方法里经 `CodeUnit` 的封装发（`emit_op`/`emit_byte`/`emit_word`；局部槽用 `emit_load_local`/`emit_store_local` 以吃到 N 短变体；跳转用 `emit_jump` + `patch_jump`--`patch_jump` **没有 target 参数**，目标恒为调用时刻的 `size()`，故同一目标的占位必须连续回填）。
7. **文档**：`reference/specs/instruction-set.md` 加表行（操作数/栈效应/语义）与相关 lowering 图；影响赋值或循环的还要改 `lowering.md`。
8. **验收**：`tests/bytecode/`（反汇编渲染、`kOpCodeCount` 哨兵）+ `tests/compile/`（端到端断言字节码形状）；语言面可见的行为另配语料（见技能 `aria-testing`）。
9. **画像配置**：按 [bench/profile 的构建命令](../../../bench/profile/README.md#快速开始)另建 `-DARIA_OPCODE_PROFILE=ON` 的版本，构建 `aria` 和 `aria_tests`，运行画像相关测试。改热指令时再跑代表性负载的画像。

## 3. 常踩的三处

- **只改了 dispatch、没改画像 switch**：普通配置不会编译画像实现。画像配置下，Clang/GCC 启用 `-Wswitch` 时会警告漏掉的枚举（别给 `fetch` 的 switch 加 `default`）；只有把警告升为错误时才会使构建失败。
- **改了指令语义、没改栈效应注释与文档**：`Eval.cpp` 的 case 注释是契约，`instruction-set.md` 是规格，两处都要动。
- **新格式只改了 Disassembler**：`opcode_profile.cpp` 的 `size_of` 会给出错的字节长，画像静默失真。
