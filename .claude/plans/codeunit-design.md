# CodeUnit 设计与实现计划

## 目标

把 `src/bytecode/CodeUnit.{hpp,cpp}` 从空骨架落地为可用的字节码容器：持有字节流、常量池、行号表，提供基础 emit API。供后续字节码编译器（AST -> CodeUnit）与 VM `run()` 使用。

依据：`docs/bytecode-instruction-set.md` §2/§7（操作数编码、CodeUnit 结构建议）、`docs/gc-implementation-plan.md` §5 Phase 3、CLAUDE.md「关键模块」。

## 已确认的设计决策

1. **代码段 `Array<u8>`**：opcode 与内联操作数统一按字节寻址（doc §2.1， supersede gc-plan 的 `Array<OpCode>`）。
2. **常量池 `AriaArray`**（即 `Array<Value>` + `trace`）：选 `AriaArray` 而非裸 `Array<Value>`，白赚 `trace(GC&)`，供未来 `ObjFunction::trace` 直接委托（doc §7：「ObjFunction::trace 标 name、常量池…不标字节码」）。
3. **行号表 RLE**：`Array<LineEntry>`，每条 `{u32 offset; u32 line}`，仅行号变化时追加。查表二分 O(log n)。
4. **行号传入：有状态 `set_line()`**：CodeUnit 持 `current_line_`，编译器每节点开头 `set_line(node.line)`；`emit_*` 隐式记录（RLE 去重）。

## 数据成员

```cpp
class CodeUnit {
    Array<u8>        code_;         // 字节流: opcode + 内联操作数 (小端)
    AriaArray        constants_;    // 常量池 (Array<Value> + trace)
    Array<LineEntry> lines_;        // RLE 行段表 (offset -> line)
    u32              current_line_; // 当前 emit 行号 (set_line 设置)
    // ...
};
```

- `LineEntry` 为简单聚合（CLAUDE.md「类初始化」：纯数据小结构用类内默认成员初始化）：
  ```cpp
  struct LineEntry {
      u32 offset = 0;
      u32 line   = 0;
  };
  ```
  `is_trivially_copyable_v` 满足 `Array<T>` 的 static_assert（默认成员初始化不影响 trivially-copyable）。

- 三个 `Array` 成员均持 `GC*` 自释放。CodeUnit 本身**不是 Object**，是 `ObjFunction` 的值成员（doc §7）；`~CodeUnit` -> `~Array` 级联释放，与 GC sweep 的「虚析构级联释放子内存」模式契合（同 ObjString long_chars_）。
- 非拷贝/非移动（继承自 Array 的 deleted copy/move），显式 `= delete` 标注。

## 构造函数

```cpp
explicit CodeUnit(GC* gc) noexcept;
```

- 取 `GC*`（与 `Array`/`AriaArray` 的 `explicit ArrayImpl(GC*)` 一致；`ObjFunction` 经 `gc.new_object<ObjFunction>(gc, ...)` 传入）。
- 按 CLAUDE.md「复杂类」：成员声明处不写默认值，初始化统一收敛进初始化列表（大括号）：
  ```cpp
  CodeUnit::CodeUnit(GC* gc) noexcept :
      code_{gc}, constants_{gc}, lines_{gc}, current_line_{0} {}
  ```

## 方法清单

### 基础 emit（用户要求的核心）

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `emit_byte` | `void emit_byte(u8 byte)` | 写 1 字节，按 `current_line_` 记 RLE 行段 |
| `emit_word` | `void emit_word(u16 word)` | 写 2 字节**小端**（低字节先），委托两次 `emit_byte`（RLE 自动去重） |
| `emit_op` | `void emit_op(OpCode op)` | 写 opcode（`static_cast<u8>(op)`），委托 `emit_byte` |

实现细节：`emit_word`/`emit_op` 委托 `emit_byte`，行号记录集中在 `emit_byte` 内调 `record_line_()`，RLE 去重使多字节指令只产生 0~1 条行段。

### 行号

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `set_line` | `void set_line(u32 line) noexcept` | 设 `current_line_`（编译器每节点调一次） |
| `current_line` | `[[nodiscard]] u32 current_line() const noexcept` | 读 `current_line_` |
| `line_for_offset` | `[[nodiscard]] u32 line_for_offset(usize offset) const noexcept` | 二分 RLE 表，返最大 `entry.offset <= offset` 的 line；空表/越界前返 0 |

`record_line_()`（私有）：
```cpp
void record_line_() noexcept {
    const usize offset = code_.size();
    if (!lines_.empty() && lines_.top().line == current_line_) {
        return; // 同行: RLE 覆盖, 不追加
    }
    lines_.push({static_cast<u32>(offset), current_line_});
}
```

### 常量池

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `add_constant` | `u16 add_constant(Value value)` | 追加，返回索引（`u16`，超 65535 断言） |
| `constant` | `[[nodiscard]] Value constant(usize idx) const noexcept` | 读常量 |
| `constant_count` | `[[nodiscard]] usize constant_count() const noexcept` | 常量数 |

- **暂不去重**（clox 去重是优化项）：先 append；ObjString 经 intern 同指针，未来加去重 hash set 不影响 API。计划里标注为 future。

### 字节码访问（供 VM / 反汇编器 / 测试）

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `code` | `[[nodiscard]] Span<const u8> code() const noexcept` + 非 const 重载 | 字节流视图 |
| `code_size` | `[[nodiscard]] usize code_size() const noexcept` | 字节流长度 |
| `byte_at` | `[[nodiscard]] u8 byte_at(usize offset) const noexcept` | 读单字节（带越界 assert） |
| `read_word` | `[[nodiscard]] u16 read_word(usize offset) const noexcept` | 小端读 2 字节（VM/反汇编器取 `u16` 操作数） |

### 回填（跳转 backpatch，前向跳转必需）

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `patch_byte` | `void patch_byte(usize offset, u8 byte) noexcept` | 原地覆写 1 字节，**不动行号表** |
| `patch_word` | `void patch_word(usize offset, u16 word) noexcept` | 原地小端覆写 2 字节，**不动行号表** |

跳转回填场景：先 `emit_op(JUMP_FALSE); auto p = code_size(); emit_word(0);` 占位，目标确定后 `patch_word(p, off)`。占位字节已在 emit 时按当时的 `current_line_` 记过行段，回填不重记。

### GC trace

| 方法 | 签名 | 语义 |
| :--- | :--- | :--- |
| `trace` | `void trace(GC& gc) const noexcept` | 委托 `constants_.trace(gc)`（code_/lines_ 无 Value，不标） |

## 文件改动

1. **`src/bytecode/CodeUnit.hpp`**：替换空骨架。includes：`common.hpp`、`bytecode/code.hpp`、`memory/Array.hpp`、`value/AriaArray.hpp`。声明 `LineEntry`（放 `aria` 命名空间，简单聚合）+ `CodeUnit` 类。声明在前，定义放 `.cpp`。
2. **`src/bytecode/CodeUnit.cpp`**：所有方法定义（构造、emit、行号、常量池、访问、回填、trace）。
3. **`tests/test_codeunit.cpp`**（新增）+ **`tests/CMakeLists.txt`** 登记。测试覆盖：emit 序列与 code_ 字节、小端 word、emit_op、set_line + RLE 行段（同行去重 / 换行追加）、line_for_offset 二分、常量池 add/read、patch_word 回填、trace 委托（配 GC + mark）。
4. `CMakeLists.txt` 源列表已含 `CodeUnit.{hpp,cpp}`，无需改。

## 不在本次范围（留 future）

- 反汇编器（doc §8 格式，独立模块）。
- 常量池去重。
- 异常记录表 `Array<TryRecord>`（doc §6.1，待 VM raise 落地时再加）。
- `ObjFunction` 子类型（持 CodeUnit，Phase 3）。

## 验证

- `clang++ -std=c++23 -I src -fsyntax-only src/bytecode/CodeUnit.cpp` 单文件语法检查。
- `cmake --build build --target aria_tests -j` + `ctest --test-dir build --output-on-failure` 跑新增 test_codeunit。
- clang-format -i 两个文件。
