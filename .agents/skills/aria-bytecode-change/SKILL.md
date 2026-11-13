---
name: aria-bytecode-change
description: 加或改一条字节码指令（OpCode）时使用。列出完整同步面：code.hpp X 表追加与枚举序、dispatch_loop 的 case（漏 case 是运行期 fatal）、opcode_profile 的平行 switch 与 size_of/records_fall_pair、反汇编渲染与 kOpCodeCount 哨兵、CodeGen/CodeUnit 发射侧、instruction-set.md 与 lowering.md、语料与画像。
---

# 加 / 改一条指令

> 场景：新增 OpCode、改某条指令的栈效应或操作数格式。规格本体在 `.agents/reference/specs/instruction-set.md`，
> 容器与 emit 见 `reference/bytecode.md`。

## 1. 定形

先在 `instruction-set.md` 里想清楚三件事：**操作数格式**（选既有 `OpFormat` 就少改两处）、**栈效应**（右侧为栈顶；`STORE_*` 必须 peek-store、留被存值）、**谁发射**（CodeGen 哪个 visit 方法，还是只给手写字节码用）。

## 2. 同步面（按序过）

1. **`src/bytecode/code.hpp` 的 `ARIA_OPCODE_LIST`**：**末尾追加**一行 `X(NAME, Format)`。枚举顺序即 opcode 数值，插中间会移动所有后续指令的编号（`kOpCodeCount`、画像输出、手写字节码全受影响）。
2. **`tests/bytecode/test_disassembler.cpp` 的 `EXPECT_EQ(kOpCodeCount, 79u)`**：X 表行数哨兵，加一条就改这个数（不改则测试红，这是它存在的意义）。
3. **`src/runtime/Eval.cpp` 的 `dispatch_loop`**：加 `case`。漏了不会编译错--落 `default: UNREACHABLE()`，**运行期 fatal**。
4. **`src/runtime/opcode_profile.cpp`**：与 dispatch 同形的逐指令 switch（`-Wswitch` 全枚举，漏 case 编译报错）；**只有引入新 `OpFormat`** 才要改 `size_of`（`default: return 3` 会把新格式静默当 3 字节）与 `records_fall_pair`（直线型判定按格式派生）。
5. **`Disassembler`**：按格式表自动分发，既有格式零改动；**只有新格式**才加分发分支。渲染回归在 `tests/bytecode/test_disassembler.cpp`。
6. **发射侧**：`CodeGen` 的 visit 方法里经 `CodeUnit` 的封装发（`emit_op`/`emit_byte`/`emit_word`；局部槽用 `emit_load_local`/`emit_store_local` 以吃到 N 短变体；跳转用 `emit_jump` + `patch_jump`--`patch_jump` **没有 target 参数**，目标恒为调用时刻的 `size()`，故同一目标的占位必须连续回填）。
7. **文档**：`reference/specs/instruction-set.md` 加表行（操作数/栈效应/语义）与相关 lowering 图；影响赋值或循环的还要改 `lowering.md`。
8. **验收**：`tests/bytecode/`（反汇编渲染、`kOpCodeCount` 哨兵）+ `tests/compile/`（端到端断言字节码形状）；语言面可见的行为另配语料（见技能 `aria-testing`）。
9. **性能**：动的是热指令（算术/比较/字段/下标/调用）就跑 `bench/profile` 的画像，别只看墙钟总数。

## 3. 常踩的三处

- **只改了 dispatch、没改画像 switch**：`-Wswitch` 会拦（前提是画像 switch 覆盖全枚举，别给它加 `default`）。
- **改了指令语义、没改栈效应注释与文档**：`Eval.cpp` 的 case 注释是契约，`instruction-set.md` 是规格，两处都要动。
- **新格式只改了 Disassembler**：`opcode_profile.cpp` 的 `size_of` 会给出错的字节长，画像静默失真。
