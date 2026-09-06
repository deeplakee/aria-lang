## 结论

将 `src/bytecode/code.hpp` 的 OpCode 枚举升级为 X-Macro 单一事实源表（双列：名字 + 操作数格式），Disassembler 改为表驱动格式分发。**VM 主循环、CodeGen、opcode 数值布局、反汇编输出全部不动**；computed goto 本次不做，仅由本表预留基建（届时跳转表只是 `ARIA_OPCODE_LIST` 的又一行消费）。

## Step 1：`src/bytecode/code.hpp` — X 表 + 生成物

1. 定义 `#define ARIA_OPCODE_LIST(X)`：63 行，每行 `X(枚举名, 格式)`，**枚举顺序与现状完全一致**（HALT=0 → RETURN，字节码布局零变化）。分组注释与行尾长注释（LOAD_LOCAL_L 长变体、MAKE_RANGE flags、INVOKE_METHOD 暂 pass 等）以 `/* */` 形式保留在表行上——**表内禁用 `//` 注释**（多行宏体内 `//` 会因反斜杠续行吞掉下一行，`-Wcomment` 警告）。
2. 从表生成 5 样（生成器宏 `ARIA_OP_*` 用完即 `#undef`，`ARIA_OPCODE_LIST` 本体保留供未来消费）：
   - `enum class OpCode : u8`（`ARIA_OP_ENUM` 生成，首值默认 0，替代显式 `HALT = 0`）
   - `enum class OpFormat : u8`：10 种格式，手写带中文注释（Simple / U8 / U16 / ConstU16 / ImmI8 / JumpFwd / JumpBack / RangeFlags / Import / Invoke）
   - `constexpr usize kOpCodeCount`（`0 + 1 + ...` 展开式）
   - `inline constexpr const char* kOpCodeNames[kOpCodeCount]`（`#name` 自动生成）
   - `inline constexpr OpFormat kOpCodeFormats[kOpCodeCount]`
3. 63 条的格式映射（已逐条核对，合计 63）：Simple 30 条（HALT/LOAD_NIL/LOAD_TRUE/LOAD_FALSE/CLOSE_UPVALUE/LOAD_INDEX/STORE_INDEX/9 条算术比较/NOT/NEGATE/POP/DUP/DUP2/PRINT/NOP/LOAD_OBJECT/THROW/RETURN）；U8 6 条（LOAD_LOCAL/STORE_LOCAL/LOAD_UPVALUE/STORE_UPVALUE/POP_N/CALL）；U16 4 条（LOAD_LOCAL_L/STORE_LOCAL_L/MAKE_LIST/MAKE_MAP）；ConstU16 13 条（LOAD_CONST/CLOSURE + 全部名字索引类 DEF_GLOBAL/LOAD_GLOBAL/STORE_GLOBAL/LOAD_FIELD/STORE_FIELD/LOAD_THIS_FIELD/STORE_THIS_FIELD/MAKE_CLASS/MAKE_METHOD/MAKE_STATIC/LOAD_SUPER_METHOD）；ImmI8 1（LOAD_IMM）；JumpFwd 5（JUMP/JUMP_TRUE/JUMP_TRUE_OR_POP/JUMP_FALSE/JUMP_FALSE_OR_POP）；JumpBack 1（JUMP_BACK）；RangeFlags 1（MAKE_RANGE）；Import 1（IMPORT）；Invoke 1（INVOKE_METHOD）。

## Step 2：`src/bytecode/Disassembler.cpp` — 表驱动格式分发

1. `dis_instruction()`：越界判断 `byte > static_cast<u8>(OpCode::RETURN)` 改为 `byte >= kOpCodeCount`（消除对枚举稠密的依赖注释）；63-case switch 缩为 10-case `switch (kOpCodeFormats[byte])`，每支把 `kOpCodeNames[byte]` 传给**现有**解码辅助函数（`simple_instruction`/`u8_instruction`/`u16_instruction`/`const_instruction`/`load_imm`/`jump_forward`/`jump_back`/`make_range`/`import_instruction`/`invoke_instruction`，全部原样复用，渲染逻辑一字不动）。
2. 删除 `name_instruction`（仅是 const_instruction 的转发，格式合一为 ConstU16）。
3. 更新函数头注释（原「不借 opcode->名查表」改为表驱动说明）；`disassemble()` 主流程与其余辅助函数不动。

## Step 3：验证

1. 改动文件 `clang++ -std=c++23 -I src -fsyntax-only` 逐个检查。
2. 全量构建 + 测试：`cmake --build build --target aria_tests -j`、`ctest --test-dir build --output-on-failure`（现有 test_codegen 的 `"JUMP_BACK"` 文本断言、tests/bytecode 全部应通过——`#name` 与原字面量逐字节相同）。
3. 反汇编输出回归：重构前先留一份 `disassemble` 输出样本（现有测试或 CLI 跑样例脚本），改后 diff 确认逐字节一致。
4. 在 `tests/bytecode/` 补一个格式分发回归测试：每格式类手搓一条指令，断言反汇编输出（10 支分发全覆盖）；新建文件则登记进 `tests/CMakeLists.txt`。
5. `clang-format -i` 改动的源文件（`AlignConsecutiveMacros` 的表对齐交给工具）。

## Step 4：文档同步（AGENTS.md 同步义务）

1. `.claude/rules/bytecode.md`：OpCode 定义方式改写（X 表位置/双列结构/5 个生成物/kOpCodeCount）、新增指令流程（X 表加一行 + VM 加 case + 文档）、表内 `/* */` 注释硬规则、未来 computed goto 消费点预留说明。
2. `.claude/reference/bytecode/bytecode-instruction-set.md`：更新 §2.1 现状描述与「尚未提取共享表」段落（行 52 附近）；§2.2 操作数位宽表标注与 OpFormat 各类别的对应关系。
3. CLAUDE.md「当前进度」节补一句（低优先级）。

## 明确不做

- VM 主循环 `run_()`、`op_symbol()`、`run_binary_numeric`：不动（computed goto 等 M3-M6 稳定且有 benchmark 后另立任务）。
- CodeGen / CodeUnit emit：不动。
- 栈效应元数据列：不加（YAGNI，X 表将来加列是向后兼容的机械改动）。