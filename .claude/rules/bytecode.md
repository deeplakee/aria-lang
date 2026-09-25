---
name: aria-bytecode
description: aria 解释器 bytecode 层模块参考：OpCode 指令集、CodeUnit 字节码容器（emit/跳转回填/异常记录表）、Disassembler 反汇编器。读写 src/bytecode/** 或涉及字节码编码、跳转占位回填、按 ip 查行号/查 try handler 时使用。
paths:
  - "src/bytecode/**"
---

# bytecode 层模块参考

指令功能/操作数位宽/栈效应的完整规格见 `.claude/reference/bytecode/bytecode-instruction-set.md`（按需 Read）。

## `bytecode/code.hpp`

**指令集单一事实源 = `ARIA_OPCODE_LIST(X)` X-Macro**：每行 `X(枚举名, OpFormat类别)`（78 条；枚举顺序即 opcode 数值，首条 `HALT` 隐式为 0）。

- 表展开生成 `OpCode` 枚举 / `kOpCodeCount`（越界判定用，替代依赖枚举稠密）/ `kOpCodeNames` / `kOpCodeFormats`（`inline constexpr` 查表，供 Disassembler 等冷路径消费；VM 热路径不查表）；`OpFormat` 是独立的操作数格式类别枚举（`Simple`/`U8`/`U16`/`ConstU16`/`ImmI8`/`JumpFwd`/`JumpBack`/`RangeFlags`/`RegU8`/`Import`）。
- **新增指令流程**：X 表加一行（选既有 OpFormat）-> AriaVM 加对应 case -> 文档同步；Disassembler 与名字/格式表零改动（仅引入新格式类别时才同步其分发 switch）。未来 computed goto 跳转表是同表再加一行消费。
- `op_symbol(OpCode)`（`inline constexpr`）：二元算术/比较指令 -> 报错消息里的源码算子记号（`ADD` 为 `"+"`），其余 opcode 取到即 `"?"`（消费方只对本组取号，别的落 `default` 表编程错误）。消费方 = VM 数值二元的类型守卫。
- **表内不放说明性注释**（中英混排难以列对齐），语义细节统一放表头注释块速览与 `bytecode-instruction-set.md`；续行符 `\` 对齐交给 clang-format（`RightAlignEscapedNewlines`）自动维护；若确需表内注释只能用块注释 `/* */`（多行宏体内 `//` 会因反斜杠续行吞掉下一行）。
- **无 `SETUP_EXCEPT`/`END_EXCEPT`**：异常走 CodeUnit 内记录表，见 `AGENTS.md`「错误处理」第 2 条。完整指令清单见 `bytecode-instruction-set.md`。

## `bytecode/CodeUnit.hpp`

`CodeUnit` 字节码容器（`ObjFunction` 的值成员，非 Object）。

- **操作数位宽事实源** `kU8OperandMax`/`kU16OperandMax`（u8/u16 操作数最大 255/65535，`namespace aria` 级 constexpr）在头内置；本类与 CodeGen 的各语义上限常量（本侧 `kMaxPopChunk`/`kMaxNLocalSlot`/`kMaxJumpOffset`/`kMaxConstantIndex`、CodeGen 侧 `kMaxArity`/`kMaxArguments`/`kMaxConstants`/`kMaxLocals`）以之为源。
- **四个容器字段直接 public 裸露**（VM/编译器/反汇编器直接操作裸字段）：`code`（`Array<u8>` 字节流，opcode + 内联操作数小端）、`constants`（`AriaArray` 常量池，白赚 `trace`）、`lines`（`Array<LineEntry>` RLE 行段表）、`try_records`（`Array<TryRecord>` 异常记录表）。
- `try_records` **按 `begin` 非降序**（CodeGen 入口预插占位 + 结尾回填保证；相等合法--内层 try 是外层体首条语句时零发射间隔），`find_try_handler` 二分前溯取最内层；`TryRecord{begin,end,handle,stack_depth}` 的 `stack_depth` 是 try 入口局部数编译期快照，unwind 据此截值栈并把异常值落 catch 参数槽。
- **仅保留有不可散落逻辑的方法**：`emit_byte`/`emit_word`/`emit_op`（带 `line`）、`emit_pop_n`（分块 <= 255）、`emit_jump`/`patch_jump`/`emit_jump_back`（占位回填，越界返 false 不写）、`emit_load_local`/`emit_store_local`（slot 1..8 发零操作数 N 短变体 `LOAD/STORE_LOCAL_k`，槽号内嵌枚举名、连续性由 code.hpp static_assert 钉住；否则通用形态 + `u16` 槽号）、`size`、`add_constant`（**只追加不去重**--按值去重收口在编译期 `FunctionCtx::add_constant`，池内无同值重复项）、`line_for_offset`（RLE 二分）、`find_try_handler`、`trace`、`disassemble`（完整签名与语义见头注释）。
- **字节码编码逻辑（emit/跳转编码/回填/分块/槽位变体）收口于此**，编译器不再自持薄包装；越界以 bool 返回交调用方翻译为 Error，本类不持 Error 语义。
- **行号无状态模型**：emit 一律带 `line`（无重载、无 `last_line_`，调用方自跟踪当前行；RLE 去重收口私有 `record_line_`）。`explicit CodeUnit(GC* gc)`（容器分配器绑定 GC）；非拷贝/非移动。

## `bytecode/Disassembler.hpp` / `.cpp`

字节码反汇编器，把 `CodeUnit` 字节流解码为可读文本（供调试/调试器/测试核对）。

- `explicit Disassembler(const CodeUnit*, StringView name)`（name 须在 `disassemble()` 期间存活）；实例持解码游标（`codeunit_` + `offset_`）。`disassemble()` 从头解码整个 `CodeUnit`（每次调用复位游标，对象可复用），静态便捷入口 `disassembleCodeUnit`。
- **静态单指令入口** `disassembleInstruction(codeunit, offset) -> String`：构造一次性 Disassembler、拨 `offset_` 到 offset、调私有 `dis_instruction()` 返回该指令文本，与 `disassemble()` 每行指令段一致；供 VM 执行跟踪 `DEBUG_TRACE_EXECUTION` 在每条指令执行前打印其反汇编，免为跟踪复制解码表。
- **输出**：表头 `== name ==` + `constants:` 小节（非空才列）+ `try records:` 小节（非空才列，逐条 `索引: [begin, end) handle=... stack_depth=...`）+ `code:` 小节（逐指令，每行：偏移 4 hex | 行号 | opcode 名 | 操作数 hex | `;` 解析注释）+ 结尾 `== end ==`。
- **解码表驱动**：底层读取 `read_u8()`/`read_u16()`（推进 `offset_`）+ 常量注释渲染（只读不推进）；名字/格式查 code.hpp X 表生成物（`kOpCodeNames`/`kOpCodeFormats`），按 10 种 `OpFormat` 分发到解码函数；越界判定用 `byte >= kOpCodeCount`；名字索引指令与 `LOAD_CONST` 同走 `ConstU16` 格式（无独立 `name_instruction`）。
- 渲染回归由 `tests/bytecode/test_disassembler.cpp` 锁定（每格式一条）。
