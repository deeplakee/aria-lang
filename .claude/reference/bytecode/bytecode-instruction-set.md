# 字节码指令集设计

aria 是**栈式字节码 VM**：所有运算经值栈完成，指令带固定格式内联操作数。本文档以 `src/bytecode/code.hpp` 现有 `OpCode` 枚举为基准，逐条整理**功能 / 操作数位宽 / 栈效应**，并给出 CodeUnit 结构、反汇编器格式、关键 lowering 与缺口分析，供后续 CodeUnit / 反汇编器 / 字节码编译器 / VM 实现参考。

> 现状：`OpCode` 已升级为 **X-Macro 单一事实源表**（`code.hpp` 的 `ARIA_OPCODE_LIST(X)`，63 条，每行 `X(枚举名, OpFormat类别)`，生成枚举 / `kOpCodeCount` / `kOpCodeNames` / `kOpCodeFormats`）；`CodeUnit` 已落地（字节流 + 常量池 + RLE 行号表 + 异常记录表 + emit/跳转编码/槽位变体收口）；操作数编码与栈效应约定已落地。本文中标「建议」「待决」者为面向实现的提案，非既成事实。

## 1. 现状与基准

### 1.1 枚举现状（以 `code.hpp` 为准）

`OpCode : u8`，共 63 条（含 2 条 `_L` 长变体），按功能分组：

| 分组 | 指令 |
| :--- | :--- |
| 停机 | `HALT` |
| 常量/字面量加载 | `LOAD_CONST` `LOAD_NIL` `LOAD_TRUE` `LOAD_FALSE` `LOAD_IMM` |
| 局部变量 | `LOAD_LOCAL` `STORE_LOCAL` `LOAD_LOCAL_L` `STORE_LOCAL_L` |
| Upvalue | `LOAD_UPVALUE` `STORE_UPVALUE` `CLOSE_UPVALUE` |
| 全局变量 | `DEF_GLOBAL` `LOAD_GLOBAL` `STORE_GLOBAL` |
| 字段 | `LOAD_FIELD` `STORE_FIELD` |
| 下标 | `LOAD_INDEX` `STORE_INDEX` |
| this 字段（优化） | `LOAD_THIS_FIELD` `STORE_THIS_FIELD` |
| 算术/比较/逻辑 | `EQUAL` `NOT_EQUAL` `STRICT_EQUAL` `STRICT_NOT_EQUAL` `GREATER` `GREATER_EQUAL` `LESS` `LESS_EQUAL` `ADD` `SUBTRACT` `MULTIPLY` `DIVIDE` `MOD` `NOT` `NEGATE` |
| 栈操作 | `POP` `POP_N` `DUP` `DUP2` |
| 输出/调试 | `PRINT` `NOP` |
| 控制流 | `JUMP` `JUMP_TRUE` `JUMP_TRUE_OR_POP` `JUMP_FALSE` `JUMP_FALSE_OR_POP` `JUMP_BACK` |
| 函数/闭包 | `CALL` `CLOSURE` |
| 类/对象 | `LOAD_OBJECT` `MAKE_CLASS` `MAKE_METHOD` `MAKE_STATIC` `LOAD_SUPER_METHOD` `INVOKE_METHOD`(预备) `MAKE_LIST` `MAKE_MAP` `MAKE_RANGE` |
| 模块导入 | `IMPORT` |
| 异常 | `THROW` |
| 返回 | `RETURN` |

`u8` 上限 256，当前 63 条，扩空间充裕。

### 1.2 与 CLAUDE.md 的同步状态

指令清单与 `.claude/rules/bytecode.md`、CLAUDE.md「文档与参考」的文档索引保持同步。当前已定决策：

- **异常机制走 CodeUnit 内异常记录表**，**不引入** `SETUP_EXCEPT`/`END_EXCEPT` 操作码：`try` 范围与 handler 由编译期生成的记录表登记，运行时按 `ip` 查表 unwind（见 §4.16/§5.9/§6.1）。
- **`MAKE_RANGE`** 已加入（区间构造，见 §4.14）。
- **跳转 `u16` 方向拆分 + 局部槽 `_L`**：跳转偏移 `u16` 无符号、方向编码于 opcode（前向 `JUMP*` `ip+=off`、后向 `JUMP_BACK` `ip-=off`），后向恒无条件（while/for/for-in 回边）；局部槽 `u8` + `LOAD_LOCAL_L`/`STORE_LOCAL_L`(`u16`)。见 §2.3/§4.12。
- **`INVOKE_METHOD`** 作为**预留指令**加入（编译器不发射、VM 命中 `not_implemented` 经 `fatal_error` 终止）：当前模型无法在编译期区分「方法调用」与「属性访问」（`obj.m` 的 `m` 是字段还是方法，运行时由实例决定），故合并「取方法 + 调用」的 `INVOKE_METHOD` 暂无发射依据，留作未来 VM 性能优化（见 §5.6/§6.2）。

## 2. 操作数编码约定（建议）

### 2.1 字节流模型

CodeUnit 的代码段是**单字节流**：1 字节 opcode 后跟若干字节内联操作数。VM 的 `ip`（指令指针）按 opcode 查「操作数格式表」决定读取几字节、如何拼。

- **代码段类型 `Array<u8>`**：opcode 以 `static_cast<u8>(op)` 存入，操作数直接写字节，让操作数字节与 opcode 统一寻址（见 §7）。
- **字节序：小端**（低位在前）。仅影响内存表示与反汇编可读性，不落盘则无跨平台问题。
- 反汇编器与 VM 遵循同一「opcode -> 操作数格式」解码约定（本节即规格）。**共享表已提取**：`code.hpp` 的 `ARIA_OPCODE_LIST(X)` 双列表（枚举名 + 操作数格式类别）展开生成 `kOpCodeNames`/`kOpCodeFormats`，反汇编器按格式表分发解码；VM 主循环仍自持 switch（热路径操作数读取内联于各 case，不查表）。新增指令的同步点 = X 表加一行 + VM 加 case；反汇编器零改动（仅引入新 `OpFormat` 类别时才同步其分发 switch）。

### 2.2 操作数位宽（建议）

下表各类位宽与 `code.hpp` 中 `OpFormat` 格式类别的对应：`u16` 常量池索引 -> `ConstU16`；局部槽 `u8`/`u16` -> `U8`/`U16`；Upvalue 与参数数与 `POP_N` -> `U8`；跳转偏移 -> `JumpFwd`/`JumpBack`；列表/映射元素数 -> `U16`；立即整数 -> `ImmI8`。`Simple`/`RangeFlags`/`Import`/`Invoke` 为反汇编渲染层面的细分（无操作数 / 预留 flags / path 注释 / name+argc 复合）。

| 操作数种类 | 位宽 | 用于 | 理由 |
| :--- | :--- | :--- | :--- |
| 常量池索引 | `u16`（2B，0..65535） | `LOAD_CONST`/全局名/字段名/`CLOSURE`/类与方法名/`IMPORT` 路径 | 模块级 chunk 常量多（串、名、函数对象），256 易超；统一 `u16` 免长短变体，简化编译器与反汇编器 |
| 局部槽号 | `u8`（1B，0..255）短 / `u16`（2B）长 | `LOAD_LOCAL` `STORE_LOCAL`（+ `_L` 变体） | 短型覆盖 <256；`slot>=256` 编译器直接发 `LOAD_LOCAL_L`/`STORE_LOCAL_L`(`u16`)，值发射时已知、无需回填 |
| Upvalue 索引 | `u8` | `LOAD_UPVALUE` `STORE_UPVALUE` | 256 upvalue 远超实际 |
| 调用参数数 | `u8` | `CALL` | 255 参数足够 |
| 跳转偏移 | `u16`（2B 无符号） | `JUMP*`（前向 `ip+=off`）/ `JUMP_BACK`（后向 `ip-=off`） | 方向编码于 opcode，各得 64KB 量程；后向恒无条件（while/for/for-in 回边），条件跳转恒前向（§2.3/§4.12） |
| `POP_N` 计数 | `u8` | `POP_N` | 块结束清理临时，单次 255 足够 |
| 列表/映射元素数 | `u16`（2B） | `MAKE_LIST` `MAKE_MAP` | 字面量可能 >255 元素；这俩不热，`u16` 免分批 lowering |
| 立即整数 | `i8`（1B 有符号，-128..127） | `LOAD_IMM` | 0/1/-1/小下标等高频小整数；大整数走 `LOAD_CONST` |

> 待决：常量索引是否走「`u8` + 长变体（`LOAD_CONST_L` 等）」clox 风格以省字节。本文建议 `u16` 统一，代价是每条常量引用多 1 字节；若实测代码段体积敏感可改长短双形态。

### 2.3 操作数位宽策略：长变体与方向拆分

局部槽与跳转这两类可能溢出的操作数，分别用不同策略保范围又省字节。

**局部槽 -- 长变体 `_L`**：`LOAD_LOCAL`/`STORE_LOCAL` 默认 `slot:u8`（2B）；`slot>=256` 时编译器直接发 `LOAD_LOCAL_L`/`STORE_LOCAL_L`（`slot:u16`，3B）。槽号在编译器分配槽时就确定，**发长无需回填**，零代价，无硬上限（`u16` = 65535 槽）。

**跳转 -- `u16` + 方向拆分（无长变体）**：偏移 `u16` 无符号，方向编码于 opcode：前向（`ip += off`）`JUMP`/`JUMP_TRUE`/`JUMP_FALSE`/`JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP`，后向（`ip -= off`）`JUMP_BACK`（仅此一条、且无条件）。依据：aria 只有 `while`/`for`/`for-in` 三种循环（文法无 `do-while`/`repeat-until`），回边恒为「循环体末尾无条件跳回条件判断处」-- 后向跳转天然恒无条件；条件跳转（`if`/`while` 条件、`&&`/`||` 短路、`match`）恒前向。故前向/后向各分得完整 `u16` 量程（64KB，约 26K 指令），无需有符号 `i16` 的 ±32KB，也无需 `_L` 长变体与分支松弛。

| 操作数 | 何时可知 | 策略 |
| :--- | :--- | :--- |
| 局部槽号 | 编译器分配槽时即知 | 长变体 `_L`(`u16`)，零回填 |
| 后向跳转偏移 | 发射时已知（回边目标先于跳转） | `u16`，`JUMP_BACK` |
| 前向跳转偏移 | 回填时才知（前向目标后于跳转） | `u16`，方向在 opcode；超 64KB 编译错误 |

> 前向跳转仍需回填（先占位、目标确定后写偏移），但只有一种宽度（`u16`），回填即定值、无短/长选择、无重排 -- 比「`i16` + 长变体 + 分支松弛」简单得多。后向跳转发射时已知，未来若需长变体也无需回填（与局部槽同性质）。代价：单条跳转量程封顶 64KB；超限（超大函数/超大模块顶层）暂报编译错误，留待未来 `JUMP*_L`。

## 3. 栈效应记号

- 栈横向写出，**右侧为栈顶**：`[a, b]` 表示 `a` 在下、`b` 在顶。
- `...` 表示无关的更深栈内容（省略）。
- 形如 `[a, b] -> [c]`：弹出 `a,b`，压入 `c`，净效应 -1。

### 3.1 STORE 系列统一「peek-store」约定（建议，待决）

文法 `expression -> assignment`，赋值是**表达式**，须产出值（`x = e` 的值即 `e` 的值；`x += e`、`++x` 返回新值，见 `compound-assignment-lowering.md` §5）。为此建议所有 `STORE_*` 统一为 **peek-store**：**只消费定位元（locator），保留被存值于栈顶**。

| 指令 | 栈效应（peek-store） |
| :--- | :--- |
| `STORE_LOCAL` / `STORE_UPVALUE` / `STORE_GLOBAL` / `STORE_THIS_FIELD` | `[v] -> [v]` |
| `STORE_FIELD idx` | `[obj, v] -> [v]` |
| `STORE_INDEX` | `[obj, idx, v] -> [v]` |

语句上下文（`exprStmt`）在 `STORE_*` 后补一条 `POP` 丢弃。此约定**消解** `compound-assignment-lowering.md` §5 末尾「STORE 留值与否未决」的备注：采用留值，§4 各序列的末尾 `[]` 应理解为「语句上下文补 `POP` 后」的形态。详见 §5.3。

> 备选：pop-store（消费被存值），表达式上下文需额外旋转/临时槽保值。peek-store 更统一，本文采之。

## 4. 指令详表

每条给出：操作数（位宽）/ 栈效应 / 语义。`无` 表无操作数。

### 4.1 停机与空操作

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `HALT` | 无 | `... -> ...` | 停止 VM 主循环（顶层执行末尾 / 致命退出） |
| `NOP` | 无 | `... -> ...` | 空操作（占位、调试断点） |

### 4.2 常量与字面量加载

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_CONST` | `idx:u16` | `[] -> [c]` | 压入常量池 `constants_[idx]`（串/大整数/浮点/函数对象） |
| `LOAD_IMM` | `n:i8` | `[] -> [n]` | 压入小整数立即数（避开常量池，0/1/-1/小下标高频）。`compound-assignment-lowering.md` 中记作 `LOAD_I` |
| `LOAD_NIL` | 无 | `[] -> [nil]` | 压入 nil |
| `LOAD_TRUE` | 无 | `[] -> [true]` | 压入 true |
| `LOAD_FALSE` | 无 | `[] -> [false]` | 压入 false |

> 注：NanBoxing 下 `Value{}` 零填充是 f64 `0.0` **非 nil**（见 `gc-implementation-plan.md` §6）。`LOAD_NIL` 必须产出 `Value::nil_val()`，不可依赖零填充。

### 4.3 局部变量

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_LOCAL` | `slot:u8` | `[] -> [v]` | 压入当前帧 `slots_[slot]` |
| `STORE_LOCAL` | `slot:u8` | `[v] -> [v]` | peek-store 到 `slots_[slot]` |
| `LOAD_LOCAL_L` | `slot:u16` | `[] -> [v]` | 同 `LOAD_LOCAL`，`slot>=256` 时用（3B，见 §2.3） |
| `STORE_LOCAL_L` | `slot:u16` | `[v] -> [v]` | 同 `STORE_LOCAL`，`slot>=256` 时用（3B） |

局部槽由编译器在函数/块作用域内分配：slot 0 为哑元（callee 占位），用户局部自槽 1 起、形参占 slot 1..n（即函数前 n 个局部）；`this` 在方法帧中约定为槽 1（见 §4.8）。

**局部区预留约定（M1 已验证）**：`CallFrame.slots` 指向帧入口的 callee（参数已躺在 `slots[1..argc+1)`），但**槽区不会被自动保留**--临时值压栈恰好会写进 `slots[k]`。故编译器须保证**每个局部槽在作为活动局部使用前已被填充**，使已填充槽位于活动栈顶之下、临时值在其上压栈，避免覆写。填充机制（编译器侧）：声明局部时**不预占**（`declare_local` 仅登记并标未初始化、不发指令），其初始化器求值产生的值（或无初始化器时一条 `LOAD_NIL`）**恰好压在该局部槽位**即完成填充（依赖「declare 与 init 原子相邻、此前所有局部已填充」不变式，故「下个局部 slot = 当前栈高」，无需 store/pop）。读取未初始化局部由读点 `load_local` 检查 `is_initialized` 报 `UninitializedVariable`，避免读到未填充槽。for-in 的 `<iter>`/pattern 无例外同用值填槽（`<iter>` 在 `iter()` 出值后 declare、值即该槽；pattern 每轮 fresh scope 值填槽），无 `LOAD_NIL` 预占、无 `STORE_LOCAL`/`POP` 回绑。顶层帧同理（无参数，各顶层局部按上述机制填充）。

### 4.4 Upvalue 与闭包

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_UPVALUE` | `idx:u8` | `[] -> [v]` | 压入当前闭包的第 `idx` 个 upvalue（开指栈槽或已关闭值） |
| `STORE_UPVALUE` | `idx:u8` | `[v] -> [v]` | peek-store 到该 upvalue |
| `CLOSE_UPVALUE` | 无 | `[] -> []` | 批量关闭所有槽址 >= 当前栈顶的开 upvalue：值迁入堆上的 `ObjUpvalue.closed`，无弹栈（弹栈由前置 `POP_N` 承担；对齐 Lua `OP_CLOSE`）。作用域退出时编译器在弹区 `POP_N` 后发射（仅当弹区含被捕获局部） |

> 三条均已落地（M4）。编译期单函数捕获上限 = u8 索引域容量 256（索引 0..255），超限报 `TooManyUpvalues`。`CLOSE_UPVALUE` 的发射点由 CodeGen `emit_pop_locals_to` 统一收口（M4，退出作用域与 break/continue 共用）：弹出比目标深度更深的局部区时整区一条 `POP_N`（被捕获局部一并计数），弹区含被捕获局部才追加一条 `CLOSE_UPVALUE`（批量关闭槽址 >= 新栈顶的开 upvalue；弹区槽已在新栈顶之上，不 push 不覆写即安全，且两指令均无分配无安全点；外层帧槽址恒低于本帧，故新栈顶之上的开 upvalue 只属弹区局部）。落点：块/循环/try 的 `end_scope` 退出、for-in per-iteration 出口（每轮新鲜绑定语义：每轮关闭旧 upvalue，下一轮捕获全新一份）、break/continue 跳出循环；此外帧退出（RETURN / unwind 丢弃区间）由 VM `exit_frame`/`close_upvalues` 整段关闭，无需指令（见 §5.9 与 runtime.md）。`STORE_UPVALUE` 的 peek-store 与 STORE_LOCAL/STORE_GLOBAL 同约定（§3.1）。

### 4.5 全局变量

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `DEF_GLOBAL` | `name:u16` | `[v] -> []` | 弹出值，以 `constants_[name]`（ObjString）为键 upsert 当前模块 globals（命中覆写；重定义属编译期语义错误 `RedefinedVariable`，运行期按定义处理；用于顶层 `var`/`fun`/import 别名） |
| `LOAD_GLOBAL` | `name:u16` | `[] -> [v]` | 按名查当前模块 globals 压入，miss 回退 VM 级 builtins 表（type/len/str/assert），再 miss 报 `UndefinedVariable` |
| `STORE_GLOBAL` | `name:u16` | `[v] -> [v]` | peek-store 到模块 globals 表；未定义报 `UndefinedVariable`（赋值不隐式创建，必须先 `var` 声明），不回退 builtins |

全局表是各 `ObjModule` 持有的 `AriaHashTable`（即 `HashTable<Value, Value, ValueHash, ValueEq>`，键为装箱的 intern `ObjString*`）。`name` 操作数是常量池中的 ObjString 索引。

### 4.6 字段（实例属性）

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_FIELD` | `name:u16` | `[obj] -> [v]` | 弹出实例，压入 `obj.name`。若 `name` 是方法，压入**绑定方法** `ObjBoundMethod`（绑 `this=obj`），供后续 `CALL` |
| `STORE_FIELD` | `name:u16` | `[obj, v] -> [v]` | peek-store：弹 `obj`，存 `obj.name = v`，留 `v` |

`LOAD_FIELD` 对方法名返回绑定方法是 `obj.m()` 调用与 for-in 迭代协议的关键（见 §5.6），使现有指令集无需 `INVOKE_METHOD` 即可表达方法调用（`INVOKE_METHOD` 仅作优化，见 §6）。

### 4.7 下标

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_INDEX` | 无 | `[obj, idx] -> [v]` | 弹 `idx, obj`，压入 `obj[idx]`（list/map/string；越界/类型不符 -> 运行时错误） |
| `STORE_INDEX` | 无 | `[obj, idx, v] -> [v]` | peek-store：弹 `idx, obj`，存 `obj[idx] = v`，留 `v` |

> 下标操作数全在栈上，无内联操作数。`LOAD_INDEX`/`STORE_INDEX` 的取数顺序（栈顶为 `idx`、其下为 `obj`）即 `compound-assignment-lowering.md` §4.3 假设的形态。

### 4.8 this 字段（优化）

`this` 在方法帧中固定占槽 1。`obj.field` 当 `obj` 恰为 `this` 时，可省去压 `this` 这一步：

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_THIS_FIELD` | `name:u16` | `[] -> [v]` | 压入 `this.name`（`this` 取自槽 1，不压栈） |
| `STORE_THIS_FIELD` | `name:u16` | `[v] -> [v]` | peek-store 到 `this.name` |

纯 `this` 表达式（`ThisExprNode`）在实例方法体内拟编译为 `LOAD_LOCAL 1`；嵌套函数引用 `this` 捕获为 upvalue（见 §5.5）。当前类/`this` 相关 visit 未落地（M5），编译器暂不发射。

### 4.9 算术 / 比较 / 逻辑

二元算术/比较：`[a, b] -> [r]`（弹 2 压 1，净 -1）。一元：`[a] -> [r]`（净 0）。均无操作数。

| 操作码 | 栈效应 | 语义 |
| :--- | :--- | :--- |
| `EQUAL` | `[a, b] -> [a==b]` | 内容相等（`value_equal`：Int/F64 跨类型 IEEE 数值、Obj 调 `Object::equals` 虚函数；-0.0==0.0 true、NaN!=NaN） |
| `NOT_EQUAL` | `[a, b] -> [a!=b]` | `EQUAL` 取反 |
| `STRICT_EQUAL` | `[a, b] -> [a===b]` | 严格相等（`value_identical`：类型严格、f64 按位、Obj 指针；1===1.0 false、-0.0!==0.0、NaN 规范化后 NaN===NaN true） |
| `STRICT_NOT_EQUAL` | `[a, b] -> [a!==b]` | `STRICT_EQUAL` 取反 |
| `GREATER` | `[a, b] -> [a>b]` | 数值比较；类型不符 -> 运行时错误 |
| `GREATER_EQUAL` | `[a, b] -> [a>=b]` | |
| `LESS` | `[a, b] -> [a<b]` | |
| `LESS_EQUAL` | `[a, b] -> [a<=b]` | |
| `ADD` | `[a, b] -> [a+b]` | 数值加；**建议**同时承载字符串拼接（`ObjString` + `ObjString`）与 list 拼接，与 `+` 重载语义一致（待决） |
| `SUBTRACT` | `[a, b] -> [a-b]` | 数值减 |
| `MULTIPLY` | `[a, b] -> [a*b]` | |
| `DIVIDE` | `[a, b] -> [a/b]` | 数值除（已定：双 Int 走整数除法（截断）、除零报 `DivisionByZero`；含 F64 走 IEEE） |
| `MOD` | `[a, b] -> [a%b]` | 取模 |
| `NOT` | `[a] -> [!a]` | 逻辑非（按真值翻转，结果为 bool） |
| `NEGATE` | `[a] -> [-a]` | 数值取负 |

> 逻辑 `&&`/`||` **短路求值**，不设独立 `AND`/`OR` 指令，经 `JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP` lowering（见 §5.2）。区间 `..`/`...` 经 `MAKE_RANGE` 发射（指令已加入 `code.hpp`，待 `ObjRange` 落地后启用编译，见 §6.3）。

### 4.10 栈操作

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `POP` | 无 | `[v] -> []` | 丢弃栈顶 |
| `POP_N` | `n:u8` | `[v1..vn] -> []` | 丢弃栈顶 `n` 个（块结束清理多个临时） |
| `DUP` | 无 | `[a] -> [a, a]` | 复制栈顶 1 槽 |
| `DUP2` | 无 | `[a, b] -> [a, b, a, b]` | 复制栈顶 2 槽（保序），对应 Python `DUP_TOP_TWO` |

`DUP`/`DUP2` 为复合赋值 / 前置 `++`/`--` 的 locator 保留而设（见 `compound-assignment-lowering.md`），亦是通用栈操作。

### 4.11 输出 / 调试

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `PRINT` | 无 | `[v] -> []` | 弹出值并输出（对应 `print` 语句；格式化用 `io::println`+`std::format`） |

### 4.12 控制流（跳转）

跳转偏移为 `u16` 无符号，方向编码于 opcode：前向（`JUMP*`）`ip += off`、后向（`JUMP_BACK`）`ip -= off`。偏移相对于**跳转指令操作数末尾的下一字节**（即 `ip` 在读完操作数后的位置）。

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `JUMP` | `off:u16` | `... -> ...` | 前向无条件跳 `ip += off` |
| `JUMP_TRUE` | `off:u16` | `[v] -> []` | 弹出 `v`；真则前向跳，假则落空 |
| `JUMP_FALSE` | `off:u16` | `[v] -> []` | 弹出 `v`；假则前向跳，真则落空 |
| `JUMP_TRUE_OR_POP` | `off:u16` | `[v] -> [v]`（真，跳）/ `[v] -> []`（假，落空） | 真则**跳且留值**（结果即 `v`），假则弹落空。用于 `||` 短路 |
| `JUMP_FALSE_OR_POP` | `off:u16` | `[v] -> [v]`（假，跳）/ `[v] -> []`（真，落空） | 假则**跳且留值**（结果即 `v`），真则弹落空。用于 `&&` 短路 |
| `JUMP_BACK` | `off:u16` | `... -> ...` | 后向无条件跳 `ip -= off`；while/for/for-in 回边、`continue` |

`_OR_POP` 变体（"命中跳，否则弹"）保证短路时被测值本身成为表达式结果（`a && b` 当 `a` 假时结果为 `a`，无需重压）；落空分支弹掉被测值，使跳/落空两路在汇合点栈深一致。`if`/`while` 等条件判断用弹出版 `JUMP_TRUE`/`JUMP_FALSE`。

各 `JUMP*` 前向、`JUMP_BACK` 后向（方向在 opcode，偏移恒 `u16` 无符号，各 64KB 量程）。后向跳转恒无条件：aria 只有 `while`/`for`/`for-in`（无 `do-while`），回边即循环体末尾跳回条件判断处，天然无条件；条件跳转恒前向（§2.3）。

### 4.13 函数调用与闭包

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `CALL` | `argc:u8` | `[callee, a1..aN] -> [r]` | 调用 `callee`（`N=argc`）。callee 为 `ObjClosure` -> 执行函数体；为 `ObjClass` -> 实例化（分配 `ObjInstance` + 调 `init`，返回实例）；为 `ObjNative`/绑定方法 -> 调原生/方法 |
| `CLOSURE` | `fn:u16` | `[] -> [closure]` | 取常量池 `ObjFunction`，创建 `ObjClosure` 并按 `fn` 的捕获描述表填 upvalue 数组（见下） |

`CALL` **重载**函数调用与类实例化：`Foo(args)` 编译为 `LOAD Foo` + `<args>` + `CALL argc`，VM 见 callee 是 `ObjClass` 即走实例化路径。故无需独立 `NEW` 指令。

**`CLOSURE` 的捕获描述（已落地，M4）**：存于 `ObjFunction` 元数据（非字节码尾随操作数）--`ObjFunction` 持 `Array<UpvalueDesc>`，每条 `{is_local: bool, index: u16}`，编译器 `compile_function` 尾部把子上下文（FunctionCtx）登记的捕获描述一次性 flush 进来。`CLOSURE fn:u16` 取常量池里的 `ObjFunction`、建 `ObjClosure` 并立即压栈（「栈即根」），VM 遍历 `fn.upvalue_descs()` 逐个建 `ObjUpvalue`（开指槽或复用外层 upvalue）：

- `is_local=true`：捕获**外层帧**的局部槽 `index`（真捕获；同槽捕获经开链复用同一 `ObjUpvalue`，「捕获即引用」）。
- `is_local=false`：捕获**外层闭包**的第 `index` 个 upvalue（穿透捕获；复制外围闭包的同下标 upvalue 指针）。

`index` 用 `u16` 与 `LOAD_LOCAL_L` 的 `slot:u16` 同域（0..65535），无捕获范围短板。捕获描述不进字节码流，`CLOSURE` 定长 3B、反汇编器线性扫即可（无需按元数据步进 `ip`）。编译器侧捕获解析见 `CodeGen::resolve_upvalue`（递归：外层局部命中 -> `{is_local=true, slot}` 并置该局部 `is_captured`；否则穿透外层 upvalue -> `{is_local=false, idx}`；容量超限报 `TooManyUpvalues`）。

### 4.14 类与对象

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_OBJECT` | (无) | `[] -> [Object]` | 压栈内置 `Object` 根类（VM 内部指针，不经名字查，避免 shadow `Object` 名破坏隐式继承） |
| `MAKE_CLASS` | `name:u16` | `[super] -> [class]` | 弹 superClass，创建 `ObjClass`（名取自常量池、`super`=弹出类），压栈。无显式父类时编译器先发 `LOAD_OBJECT` |
| `MAKE_METHOD` | `name:u16` | `[class, closure] -> [class]` | 弹 `closure`，作为方法 `name` 注册到 `class`（**静态方法与实例方法皆经此注册**，含 `init`）；区别仅在闭包是否绑 `this`：静态方法无 `this`、经 `ClassName.x` 访问静态；实例方法 `this` 占槽 1。`init` 由 init 缓存按名查（§5.5）；`class` 留栈继续接收成员 |
| `MAKE_STATIC` | `name:u16` | `[class, value] -> [class]` | 弹 `value`，作为静态变量 `name` 存入 `class`（`var` 声明 lowering：eager 求值初始化器后存）；`class` 留栈继续接收成员 |
| `LOAD_SUPER_METHOD` | `name:u16` | `[] -> [bound]` | `this` 取自槽 1，父类取自**当前闭包的 defining class**（`ObjFn.defining_class.super`，编译期绑定、不经栈）；查方法 `name` 绑成 `ObjBoundMethod` 压栈供 `CALL` |
| `INVOKE_METHOD` | `name:u16`, `argc:u8` | `[obj, a1..aN] -> [r]` | **预留指令**（编译器不发射、VM 命中 `not_implemented`）：合并「取方法 `name` + `CALL argc`」，直接在实例上查方法并调用。因当前无法编译期区分方法调用与属性访问，编译器暂不发射；语义等同 `LOAD_FIELD name`（返绑定方法）+ `CALL argc`，留作性能优化（见 §5.6/§6.2） |
| `MAKE_LIST` | `n:u16` | `[v1..vn] -> [list]` | 取栈顶 `n` 个为元素创建 `ObjList`（保序），压栈 |
| `MAKE_MAP` | `n:u16` | `[k1,v1..kn,vn] -> [map]` | 取栈顶 `n` 对 `(k,v)` 创建 `ObjMap`，压栈 |
| `MAKE_RANGE` | `flags:u8` | `[lo, hi] -> [range]` | 取栈顶 `lo, hi` 创建 `ObjRange`；`flags` 编码含/不含上界（`..` 含、`...` 不含）。供 `for-in` 遍历区间（`ObjRange` 实现迭代协议） |

def 声明 lowering：先装载父类入栈（显式 `LOAD_GLOBAL "Bar"`；无父类时发 `LOAD_OBJECT` 装 `Object`），`MAKE_CLASS` 弹父类创建类；随后按成员出现顺序发射--静态变量（`varDecl`）求值初始化器后经 `MAKE_STATIC` 存入类；静态方法（`funDecl`）与实例方法（`function`，含 `init`）各发 `MAKE_METHOD`，`class` 始终留栈；末尾 `STORE_GLOBAL`/`STORE_LOCAL` 绑定类名，或 `POP` 丢弃（见 §5.5）。`init` 不用专用指令、由 init 缓存按名查（§5.5）。`def` 在运行时仍是 `ObjClass`，OpCode 名（`MAKE_CLASS` 等）与 `ObjType::CLASS` 不随关键字改名。

### 4.15 模块导入

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `IMPORT` | `path:u16` | `... -> [module]` | 解析 `path` 并复用/加载模块，把 `ObjModule` 压栈。绑定交后续 `DEF_GLOBAL`/值填槽按作用域走 |

`path` 为常量池 ObjString 索引。模块解析、路径搜索、循环导入检测留 VM/嵌入层。`IMPORT` 仅负责取模块对象压栈；绑定由 CodeGen 按作用域走——顶层经 `DEF_GLOBAL alias`（弹值定义全局）、嵌套经值填槽（`IMPORT` 压在 `declare_local` 的 slot 即该局部）+ `mark_initialized`，与 `var`/`fun` 同形 lowering。故 `IMPORT` 不带 `alias` 操作数。

### 4.16 异常

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `THROW` | 无 | `[v] -> ` | 弹出 aria 值 `v` 作为异常抛出（原值入挂起寄存器不包，catch 绑原值保类型）；`unwind` 查各帧 `try_records` 回退到最近 `try` handler。控制流转移，栈由 unwind 重建 |

> **异常机制走 CodeUnit 内记录表，不设 `SETUP_EXCEPT`/`END_EXCEPT` 操作码**：`try` 块的范围与 handler 地址由编译期生成的**异常记录表**（存于 CodeUnit）登记，每条记录含 `try` 起始/结束 `ip` + `handler ip` + `stack_depth`（帧深度由 unwind 遍历隐式决定、catch 参数槽由 handler 约定承载，均不入记录）。`THROW`/`raise` 时按帧 `last_ip` 反推 offset 查表，定位最近覆盖当前指令的 `try` 记录，按记录 `truncate` 回退并跳到 handler。比操作码方案更紧凑（不污染字节码流、无需每进/出 `try` 发指令）。M3 已落地：记录表结构与查表（`TryRecord{begin,end,handle,stack_depth}` + `find_try_handler` 返命中记录指针）+ CodeGen try/catch/throw 发射 + VM `unwind`（查表 + 逐帧回退 + 截栈跳 handler + 未捕获物化 Error 带堆栈跟踪）全链可用（见 §5.9/§6.1；`finally` 已裁撤、善后后继 defer 已降为可选后续不绑定 M4）。

### 4.17 返回

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `RETURN` | 无 | `[v] -> ` | 从当前函数返回 `v`（无返回值时编译器先发 `LOAD_NIL`）。退出当前 `CallFrame`，把 `v` 压入调用者栈顶，`ip` 恢复到 `CALL` 之后 |

`RETURN` 也用于顶层执行结束：主模块体（入口名 `<main>`）返回值即程序结果；导入模块体帧（函数名 == `<module>`，由 IMPORT 加载层驱动 run-once）**弹弃返回值**、改压模块对象（「模块体返回模块」），故 `IMPORT` 命中/未命中栈效应统一 `[..., module]`。`HALT` 与顶层 `RETURN` 的分工：`HALT` 彻底停机，`RETURN` 仅退一帧。

## 5. 关键 lowering

### 5.1 if / while / for / for-in

```
# if (cond) then else elseB
<cond>                 ; [c]
JUMP_FALSE L_else      ; []
<then>                 ; [...]
JUMP L_end
L_else:
<elseB>
L_end:

# while (cond) body
L_start:
<cond>                 ; [c]
JUMP_FALSE L_end       ; []
<body>
JUMP_BACK L_start
L_end:
# break -> JUMP L_end ; continue -> JUMP_BACK L_start

# for (init; cond; incr) body
<init>                 ; varDecl: 分配局部; exprStmt: <e> + POP
L_start:
<cond>                 ; 省略 -> 恒真, 跳过此段
JUMP_FALSE L_end       ; []
<body>
<incr>                 ; <e> + POP
JUMP_BACK L_start
L_end:
# continue -> JUMP L_incr (有 incr: 前向跳, 回填须先于 incr 发射; 无 incr -> JUMP_BACK L_cond); break -> JUMP L_end
```

for-in 依赖迭代协议（`iter`/`has_next`/`next`），靠方法调用表达（见 §5.6）：

```
# for (pat in iterable) body
<iterable>             ; [obj]
(call obj.iter())      ; [it]    经 LOAD_FIELD "iter" + CALL 0, 或内建
L_start:
(call it.has_next())   ; [bool]
JUMP_FALSE L_end       ; []
(call it.next())       ; [v]     绑入 pat (identifier: 值填槽 declare 不发指令; "_": POP 丢弃)
<body>
JUMP_BACK L_start
L_end:
```

### 5.2 短路逻辑 `&&` / `||`

```
# a && b   (a 假则结果为 a, 否则结果为 b)
<a>                    ; [a]
JUMP_FALSE_OR_POP L_end ; [a] (a 假跳, 留 a) / [] (a 真落空)
<b>                    ; [b]
L_end:                 ; 结果在栈顶

# a || b   (a 真则结果为 a, 否则结果为 b)
<a>                    ; [a]
JUMP_TRUE_OR_POP L_end  ; [a] (a 真跳, 留 a) / [] (a 假落空)
<b>                    ; [b]
L_end:
```

### 5.3 赋值 / 复合赋值 / 前置 `++`/`--`（peek-store 形态）

采用 §3.1 的 peek-store 约定后，`compound-assignment-lowering.md` §4 各序列末尾留值（语句上下文补 `POP`）。以 `obj.f += e` 与 `++obj[idx]` 为例：

```
# obj.f += e  (表达式上下文, 结果为 新值)
<obj>                  ; [obj]
DUP                    ; [obj, obj]
LOAD_FIELD "f"         ; [obj, obj.f]
<e>                    ; [obj, obj.f, e]
<op>                   ; [obj, newval]      newval = obj.f op e
STORE_FIELD "f"        ; [newval]           peek-store: 弹 obj, 留 newval
# 语句上下文在此补 POP -> []

# ++obj[idx]  (返回新值)
<obj>                  ; [obj]
<idx>                  ; [obj, idx]
DUP2                   ; [obj, idx, obj, idx]
LOAD_INDEX             ; [obj, idx, obj[idx]]
LOAD_IMM 1             ; [obj, idx, obj[idx], 1]
ADD                    ; [obj, idx, newval]
STORE_INDEX            ; [newval]           peek-store: 弹 obj,idx, 留 newval
```

`--E` 把 `ADD` 换 `SUBTRACT`；标识符 / this 字段形式按 §4.3/§4.8 无 `DUP` 或省 `obj`。详见 `compound-assignment-lowering.md`（本文仅以 peek-store 更新其 tail 语义）。

### 5.4 闭包 / upvalue

已落地（M4）。具名 fun / lambda 一律经 `CLOSURE` 上栈（`CLOSURE` 自取常量池 `ObjFunction`，无前置 `LOAD_CONST`--旧 lowering 的 `LOAD_CONST fn_idx + CLOSURE` 双发属残留，定夺为仅 `CLOSURE`）：

```
# 外层函数内定义闭包, 捕获外层 local x (slot 3) 与外层 upvalue u (idx 1)
# (捕获描述 [{is_local=true, index=3}, {is_local=false, index=1}] 存于 fn 的 UpvalueDesc 表,
#  编译期 flush 进 ObjFunction 元数据)
CLOSURE fn_idx         ; [closure]   ; VM 读 fn.upvalue_descs() 建捕获, 无尾随操作数
# 块结束、x 将销毁且被捕获时: POP_N 整区弹局部 + CLOSE_UPVALUE 批量关闭槽址 >= 新栈顶的开 upvalue
# (CodeGen emit_pop_locals_to 发射, 仅当弹区含被捕获局部, 落点见 §4.4);帧退出(RETURN/unwind)
# 由 VM close_upvalues 整段关闭,无指令
```

### 5.5 def / 静态成员 / 实例方法 / super / 实例化

def 体内三种成员：`var` -> 静态变量（属类，`ClassName.x` 读写）；`fun name(){}` -> 静态方法（无 `this`，经 `ClassName.x` 访问静态成员）；`name(){}`（裸 identifier）-> 实例方法（`this` 占槽 1，经 superclass 链继承、`super.m()` 可用）。体内裸名解析顺序：**局部 -> upvalue -> 模块全局**（类静态不在裸名作用域，经 `ClassName.x` 限定访问）。`def` 在运行时仍是 `ObjClass`。

```
# def Foo : Bar { var x = 1; fun s() {...} init(...) {...} m(...) {...} }
LOAD_GLOBAL "Bar"      ; [super]    ; 显式父类（def Foo 无父类时改发 LOAD_OBJECT）
MAKE_CLASS "Foo"       ; [class]    ; 弹 super 创建 Foo
LOAD_IMM 1             ; [class, 1] ; var x = 1
MAKE_STATIC "x"        ; [class]    ; 存为静态变量 x
MAKE_METHOD "s"        ; [class]    ; 静态方法: 闭包不绑 this
MAKE_METHOD "init"     ; [class]    ; 实例方法: init, this 占槽 1（init 缓存按名查）
MAKE_METHOD "m"        ; [class]    ; 实例方法
STORE_GLOBAL "Foo"     ; []         ; 绑类名 (或 STORE_LOCAL)

# Foo(args)  实例化
LOAD_GLOBAL "Foo"      ; [class]
<args>                 ; [class, a1..aN]
CALL argc              ; [instance] ; VM 见 ObjClass -> 新建 ObjInstance + 调 init

# super.m(args)
LOAD_SUPER_METHOD "m"  ; [bound]    ; this 来自槽 1, 父类来自当前闭包的 defining class
<args>                 ; [bound, a1..aN]
CALL argc              ; [r]
```

静态变量**创建**经 `MAKE_STATIC`（eager 求值初始化器后存，见上文 lowering）；`Foo.x` 静态访问、`foo.x` 实例优先+静态回退（遮蔽）的 lowering 待 VM 精化：静态变量可视为类对象的字段（复用 `LOAD_FIELD`/`STORE_FIELD` 于 `ObjClass`），`foo.x` 在 `LOAD_FIELD` 未命中实例属性时回退查类静态。具体栈模式（`STORE_FIELD` 的 peek-store 与「class 始终留栈」的衔接，可能需 `DUP`/`POP` 或专用指令）留编译器实现时定；裸名解析见下文作用域模型（`LOAD_GLOBAL` 查模块全局，类静态经 `ClassName.x` 限定）。

静态成员继承与缓存（编译期/VM 语义）：静态变量与静态方法经 `ObjClass.superclass_` 链继承（与实例方法分派同一机制、复用同一指针）。类成员读写取 Python/JS class attributes 语义（读穿透、写遮蔽，M5 决策补记 2026-09-09）：子类未重声明时读沿链穿透命中父类槽；类上赋值 `Sub.x = v` 落**接收类自身**表（继承名新建遮蔽键、本类已有原槽更新），沿链全 miss 的新名字拒（无 monkey-patch），方法槽亦允许改写（bound 缓存取首解析快照；"init" 赋值同步 `ObjClass.init_`）。类静态经 `ClassName.x` 限定访问（运行期走 super）、不在裸名作用域（见下文作用域模型）。方法查找靠内联缓存 / bound method 缓存加速：`obj.m` 返回绑实例的 bound method 对象，可缓存在实例查找表；闭包不可变，缓存安全。

Object 根类（编译期/VM 语义）：所有用户类隐式继承内置 Object 根类，统一继承链语义。**Bootstrap**：VM init 阶段、用户代码执行前，创建 Object--一个普通 `ObjClass`，`superclass_ = nil`（唯一 nil 超类的类），VM 成员单独持有（M5 决策 3：**不进 globals/builtins/任何模块**，裸名解析四层均够不到、正常代码访问不到，本段旧文「注册为全局名」措辞待阶段 4 随 §5.5 精化收回）。**编译器隐式超类**：`def Foo`（无显式父类）编译等价于 `def Foo : Object`，编译器填 Object 作隐式超类；`def Foo : Bar` 设 super=Bar（经 Bar 链最终到达 Object）。隐式 Object 引用经 `LOAD_OBJECT`（VM 内部指针，**不按名字查**）--Object 不入任何名字空间，用户 shadow 全局名无从谈起，继承机制天然免疫用户命名。**Object 方法集**：保持最小通用--`init`（no-op，返回 this）、`to_string`（如 `<ClassName>`），可再加 `equals`（引用相等）、`hash`（地址/id）、`class`（返 ObjClass）、`is_a(Class)`；每个方法被所有实例继承，谨慎加。Object **不带静态变量**（根类保持最小，静态会被全类经链共享）。Object 方法体内 `super` **非法**（它是根，无超类），编译期/VM 报错。`Object()` 实例化允许（permissive），产出持 Object 方法的最小 ObjInstance。**边界**：Object 统一的是用户定义类的实例（持 ObjClass 的 ObjInstance）。原始值（nil/bool/f64/int，NaN-boxing 内联载荷，非 Obj）不在 Object 层次内；内置 Obj 类型（ObjString/ObjList/ObjMap 等，C++ 类型带 ObjType tag、不持 ObjClass）暂亦不在内；纳入内置类型为 uniform OOP 目标方向（见下文）。链式查找（静态/实例方法/`foo.x` 回退/`init` 解析）统一终止于 Object（裸名走模块全局，不在此列）。

`init` 缓存与实例化快路径（编译期/VM 语义）：`init` 是每次构造的热路径，不在实例化时查表，而在 MAKE_CLASS 注册完所有成员后**当场解析一次**--沿 `superclass_` 链找到最近的 `init` 闭包，存进 `ObjClass.init_`。之后 `Foo()` 实例化直接读 `class.init_`，O(1) 不走链。有 Object 根兜底，`init_` 永不为空（总在 Object 命中 no-op `init`）。进一步快路径：Object 的 `init` 是 no-op，若 `class.init_` 指向的就是 Object 的 no-op `init`（指针同一性判断）且无参（`argc == 0`），实例化**连调都不调**，直接 `new ObjInstance(class)` 返回--与无 Object 根时一样快，语义统一不付性能代价。aria 实例字段由 `init` 内 `this.x = ...` 动态设置，无自定义 `init` 的类本无字段要初始化，跳过 no-op 留空 ObjInstance 正确。缓存失效：`def` 一次性定义方法集；类上赋值可改写方法槽（读穿透、写遮蔽，M5 决策补记）--已绑定实例不失效，bound 缓存取**首解析快照**语义（新解析见新闭包、旧实例沿用旧绑定）；"init" 赋值由 STORE_FIELD 类路径特例同步 `init_`（值非 ObjClosure 拒写），故实例化快路径前提（`init_` 与类表 init 槽一致）始终成立。

**作用域模型：模块即命名空间 + 裸名走词法+模块全局**（编译期/VM 语义，Python/JS 路子）：

- **模块 = 命名空间（非类）**：文件是一个模块，顶层 `var`/`fun`/`class` 是**模块级绑定**（模块全局），不是某个类的静态；顶层可执行语句编进模块体（`ObjFn`），导入时跑一次（run-once）。模块存于 VM 内部模块表，不是某个 root 类的静态。
- **ObjClass 两指针**：`super`（继承）、`meta`（元类，uniform OOP 未来）。**无 enclosing 指针**--类不记词法外层。**两根**：`Object`（`super=nil`，继承终止）、`meta`（`meta=meta` 自指，元类终止，未来）。
- **裸名解析（词法 + 模块全局，无类静态）**：编译期判定局部/参数 -> `LOAD_LOCAL`、upvalue -> `LOAD_UPVALUE`；否则发 `LOAD_GLOBAL`，运行期查**当前模块的全局表**。**不走 enclosing+super，不查类静态**--类静态不在裸名作用域。未命中则全局未定义（报错）。裸名只看：局部、upvalue（外围函数局部）、模块全局。
- **裸名赋值与声明（写路径，与读对称）**：裸名赋值 `x = v`（无 `var`）走与读相同路径（`STORE_LOCAL` / `STORE_UPVALUE` / `STORE_GLOBAL` 查模块全局表），命中即写，未命中报错（不隐式创建，必须先 `var` 声明）。`STORE_UPVALUE` 与 `LOAD_UPVALUE` 对称（闭包变异）。`var` 声明：**函数体**=局部、**模块顶层**=模块全局、**类体**=静态（经 `ClassName.x` 访问，不进裸名）。实例字段写经 `STORE_FIELD`（`this.x = v` / `obj.x = v`，动态创建）。
- **类静态访问（限定，运行期）**：`ClassName.x`--解析 `ClassName`（裸名，通常是模块全局或经全路径），再沿其 super 链查静态（含继承，共享槽语义），运行期。`this.x` / `obj.x`--实例访问，先查实例字段，未命中回退类静态（沿实例 class 的 super 链）。**裸名看不到类静态**，必须限定。
- **嵌套类（全路径）**：`class B` 在 `class A` 内 -> `B` 是 `A` 的静态 `A.B`。从内部引用 `B` 须经全路径 `A.B`（限定访问，运行期查 `A` 的静态 `B`），**无 enclosing 链、无裸名**。模块顶层类是模块全局，裸名可访；嵌套类不是模块全局，须全路径。
- **无 `This` 关键字**：自引用写类名（`Foo.x`）。
- **ObjFn 持 defining class**：用于 `super`（`LOAD_SUPER_METHOD` 的“父类来自当前闭包”）。**不再用于 enclosing 走链**（无 enclosing）。
- **`this`（实例，小写）为捕获 upvalue（arrow-function 语义）**：`this` 是实例方法的槽 1；嵌套函数引用 `this` 时，沿外围函数帧找最近的**实例方法**，把它的槽 1 捕获为 upvalue（`this` -> `LOAD_UPVALUE`，`.x` -> `LOAD_FIELD`）。若链上无实例方法（静态方法、顶层函数、或只嵌在静态方法里），`this` 不可用--编译期报错。静态方法本身无 `this`。
- **`super` 边界**：`super.m()` 仅实例方法可用--用槽 1 的 `this` 调被覆写的实例方法（`LOAD_SUPER_METHOD` 从 defining class 的 super 起）。访问父类静态直接 `ClassName.x`，**不支持 `super.x`**（避免额外复杂度）。静态方法无 `this`，`super` 非法。
- **静态初始化时机（eager）**：静态变量的初始化器在类定义时求值（eager），非首次访问（lazy）。故 `class A { var x = B(); }` 要求 `B` 先于 `A` 定义；类定义顺序即静态初始化顺序。
- **动态加静态**：不支持 monkey-patch；`Foo.newStatic = v`（新名）报错，静态必须 `var` 声明。实例字段动态（`this.x = v` 创建），静态声明式--有意不对称（`var` 标静态、与实例方法声明区分）。
- **缓存**：`LOAD_GLOBAL`（模块全局查表）默认不缓存，内联缓存（per call-site）留作后续优化；`init` 缓存见上文本节。

**模块导入**：文件是模块（非类）。解释器启动时定**源根列表**（source root list）--解释器标准库 `lib` 路径 + 入口文件所在目录（环境变量源根留待后续）；每个文件记住自己所属的源根。导入只用字符串路径，**不支持裸名 `import foo`**：`import "lib/utils" as Utils`（绝对，从源根列表搜 `lib/utils.aria`，加载该模块、跑其模块体 run-once）、`import "./utils" as U` / `import "../lib/x" as X`（相对当前文件目录）、`import "lib" as Lib`（目录包导入，`Lib` 绑该包、跑其 index 模块体若有）。`import "path" as alias` **强制 `as alias`**--绑模块到 `alias`（当前作用域变量：模块顶层=模块全局、函数体/块内=局部；`IMPORT` 压模块值于栈顶，绑定经 `DEF_GLOBAL`（顶层）或值填槽 + `mark_initialized`（嵌套）按作用域走），不解析路径算模块名、必须显式起别名。**相对导入不得越出当前文件所属源根**--`../` 爬到源根之上即报错（源根外文件无自然模块路径；要引用源根外文件用绝对导入命中其他源根，或把目录加进源根列表）。**目录 = 包**（模块查找路径结构，非嵌套类）：`lib/utils.aria` 是包 `lib` 下的模块 `utils`，`import "lib/utils" as Utils` 找到它；`import "lib" as Lib` 导入整个包。允许循环导入--命中正在初始化的模块返回半初始化对象。裸名不触发自动导入，兄弟模块须显式 `import "./sibling" as Sibling`。边界：符号链接按规范路径判定（源根内 symlink 指向外部仍算越出）；重叠源根按列表顺序首次命中。标记文件（`aria.toml`/`.ariaroot`）作源根是未来 `aria run` 项目级执行的特性，暂不支持。

**目标方向：uniform OOP（Design B）**：aria 的方向是把内置类型也纳入 aria 类体系--每个内置 Obj 类型配一个 `ObjClass`（String/List/Map 继承 Object），`class_` 进 Object 头，方法统一走 `class_->lookup` 分派，内置类型继承 Object 的公共方法。原始值（nil/bool/f64/int）进一步可经 tag->class 映射（Num/Bool 类）获得方法分派而不装箱（Wren 路子），仍保 NaN-boxing 内联存储。性能上走 CPython 式 intrinsic 做热路径--统一语义为规约、intrinsic 为快路径，二者兼得；`+8B class_` 只落 Obj、不落原始值，代价 bounded。此为方向性目标，尚未实现；落地前可先用 Design A（内置保持特殊、虚 `op_get_field`）作 interim，不阻塞迁移（`op_get_field` 接口保持、内部从虚派发换类表查找）。

### 5.6 方法调用与 for-in（无 `INVOKE_METHOD` 也能表达）

`obj.m(args)` 经「`LOAD_FIELD` 返回绑定方法 + `CALL`」两步：

```
<obj>                  ; [obj]
LOAD_FIELD "m"         ; [bound]   ; m 是方法 -> ObjBoundMethod(this=obj)
<args>                 ; [bound, a1..aN]
CALL argc              ; [r]
```

`INVOKE_METHOD`（合并 `LOAD_FIELD`+`CALL`，少一次压绑方法、省一次 dispatch）已作为**预留指令**加入：当前模型无法编译期区分方法调用与属性访问，编译器暂不发射、VM 命中 `not_implemented` 终止；现状靠 `LOAD_FIELD`+`CALL` 保证正确性（见 §6.2）。for-in 的 `iter`/`has_next`/`next` 同此路径。

### 5.7 match（糖 -> if-else 链）

match 无独立指令，按 arm 顺序生成「求值模式 -> `EQUAL`（== 内容相等，见 §4.9）-> 命中跳转」链，subject 须跨 arm 保留：

```
<subject>              ; [s]
# arm 1 (模式 p1)
<p1>                   ; [s, p1]
EQUAL                  ; [bool]
JUMP_TRUE L_arm1       ; []  (弹 bool; s 保留)
# arm 2 ... 同上
JUMP L_default         ; 无匹配 (无 "_" -> 运行时错误)
L_arm1:
POP                    ; []  (丢弃 subject)
<body1>
JUMP L_end
L_default:             ; "_" 兜底 arm
POP                    ; []
<body_default>
L_end:
```

matchExpr 同理，分支体为表达式（留值），需在各 arm 末尾统一栈深度。

### 5.8 解构赋值 `listPattern = expr`

```
# [a, _, c] = rhs
<rhs>                  ; [list]
LOAD_IMM 0; LOAD_INDEX ; [list, list[0]]  -- 绑 a: STORE_LOCAL a; POP list[0]? 
# 实践: 每个位置 LOAD_IMM i + LOAD_INDEX + STORE_local, "_" 跳过
# rest: 收集 list[i..] 为新 list (需切片, 待内建/指令支持)
```

> rest 收集 `list[i..]` 可能需内建 `slice` 或专用指令；当前指令集靠 `LOAD_INDEX` 逐个取，rest 部分待补（见 §6.4）。

### 5.9 try/catch（CodeUnit 内异常记录表方案）

不设 `SETUP_EXCEPT`/`END_EXCEPT` 操作码。编译期为每个 `try` 在 CodeUnit 的异常记录表生成一条记录：

```
记录表条目: { begin, end, handle=L_catch, stack_depth }   -- 帧深度由 unwind 遍历隐式决定, catch_slot 由 handler 约定承载
```

字节码本身不含 try 设置/清理指令，`<body>` 直接顺序排列：

```
<body>                 ; try 体内, 顺序执行
JUMP L_end            ; 正常完成: 跳过 catch
L_catch:               ; 抛出时 VM 经 raise 查表 unwind 到此; unwind 截栈后 push 异常值,
                       ; 恰落 catch 参数槽(== stack_depth, 值填槽, 无 STORE_LOCAL)
  <catch_body>
L_end:
# 未捕获的异常继续 raise (VM 在帧耗尽时终止)
```

`raise` 流程（M3 已落地，`AriaVM::unwind()`）：按帧 `last_ip` 反推 offset -> 自最内帧向外逐帧查当前 CodeUnit 记录表找最近覆盖该指令的条目 -> 未命中的帧 `exit_frame` 逐个弹出（顺路收集未捕获跟踪三元组）-> 命中帧处值栈 `truncate_stack(slots + stack_depth)` -> 压异常值（落 catch 参数槽）-> `ip = handle`；全帧未命中物化 `Error`（反提寄存器载荷）并烘焙外->内逐帧 `at` 堆栈跟踪。`finally` 已裁撤（2026-09，善后后继 defer 已降为可选后续、不再绑定 M4），unwind 流程不再含 finally 汇合点。

## 6. 缺口分析（相对文法与 CLAUDE.md）

### 6.1 异常记录表（已采纳方案，M3 已落地）

**决定不引入 `SETUP_EXCEPT`/`END_EXCEPT` 操作码**，改用 CodeUnit 内异常记录表。`code.hpp` 仅有 `THROW`（足够：抛出动作本身只需 `THROW`，try 的范围/handler 由记录表登记，无需进/出 try 的指令）。文法 `tryCatchStmt` 已解析。已落地全链：`CodeUnit` 持 `Array<TryRecord> try_records`，每条 `TryRecord{begin, end, handle, stack_depth}`（按 `begin` 非降序，CodeGen 入口预插占位 + 结尾回填保证；`stack_depth` = try 入口局部数编译期快照，供值栈回退），`find_try_handler(ip)` 二分查最近覆盖 `ip` 的 try 记录，返 `Opt<const TryRecord*>` 指向命中记录；VM `unwind` 查表 + 逐帧回退 + 截栈跳 handler + 未捕获物化带堆栈跟踪（§5.9）；`THROW` 运行时语义（弹值入寄存器 + unwind，catch 绑原值保类型）；CodeGen try/catch（含嵌套）/throw 发射。待实现项：

- `finally` -- 已裁撤（2026-09）：从文法移除、不再有 `NotImplemented` 占位；善后后继 defer 已降级为可选后续（其他功能完成后另定，不再绑定 M4），defer 的 unwind 途中执行语义届时细化。

详见 CLAUDE.md「错误处理」第 2 条。

### 6.2 `INVOKE_METHOD`（预留指令，编译器不发射）

`obj.m(args)` 当前需 `LOAD_FIELD`(返回绑定方法) + `CALL` 两步。`INVOKE_METHOD name argc` 合并二者：直接在实例上查方法并调用，省一次「压绑方法对象 + 弹绑方法对象」与一次 dispatch。**已作为预留指令加入 `code.hpp`**，但编译器不发射、VM 命中 `not_implemented`（`fatal_error` 终止）：当前模型无法在编译期区分「方法调用」与「属性访问」（`obj.m` 的 `m` 是字段还是方法，运行时由实例决定），编译器无依据发射 `INVOKE_METHOD`，留作未来 VM 性能优化。现状靠 `LOAD_FIELD`+`CALL` 保证正确性（见 §5.6）。

### 6.3 `MAKE_RANGE`（已加入）

文法 `range -> term (".."|"...") term`，AST 有 `RangeExprNode`，`ObjType::RANGE` 已预留。`MAKE_RANGE flags:u8`（`[lo, hi] -> [range]`，`flags` 编码含/不含上界）**已加入 `code.hpp`**（见 §4.14）。待 `ObjRange` 落地后即可编译区间表达式；for-in 遍历区间由 `ObjRange` 实现迭代协议。步长等扩展留内建或后续指令。

### 6.4 内建函数与 rest 切片

- 内建已落地：VM 级只读 builtins 表（`AriaVM::builtins_`，构造期 `register_builtins` 一次性填充 type/len/str/assert）+ `LOAD_GLOBAL` 模块 globals 未命中后回退查表，不引入 `LOAD_BUILTIN` 指令（见 `.claude/rules/runtime.md` 与 vm-design.md §7）。
- 解构 `rest` 收集 `list[i..]` 需切片能力，可由内建 `slice` 或 `MAKE_RANGE`+下标协议承载。

### 6.5 迭代器

`ObjType::ITERATOR` 已预留。for-in 经方法调用（§5.6）实现，无需独立迭代指令；`iter`/`has_next`/`next` 作为方法/内建提供。

## 7. CodeUnit 结构

```cpp
class CodeUnit {
    Array<u8>        code;        // 字节流: opcode + 内联操作数 (小端)
    AriaArray        constants;   // 常量池 (Array<Value> + trace)
    Array<LineEntry> lines;       // RLE 行段表 (offset -> line)
    Array<TryRecord> try_records; // 异常记录表 (按 begin 单调, 见 §6.1)
};
```

四个容器字段直接 public 裸露，VM/编译器/反汇编器直接操作；字节码编码逻辑（`emit_op`/`emit_byte`/`emit_word`/`emit_pop_n`/`emit_jump`/`patch_jump`/`emit_jump_back`/`emit_load_local`/`emit_store_local`）收口于 CodeUnit 方法，越界以 bool 返回交调用方翻译。详见 `.claude/rules/bytecode.md`。

- **`code` 用 `Array<u8>`**：opcode 与操作数统一按字节寻址，VM 读操作数直接取字节。
- **`constants` 用 `AriaArray`**（`Array<Value>` + `trace`）：白赚 `trace(GC&)`，`ObjFunction::trace` 直接委托；`LOAD_CONST idx` 等以此索引。ObjString 经 intern 驻留，等价内容共享同一 `ObjString*`。
- **行号表 `lines`**：RLE 压缩的 `Array<LineEntry{offset,line}>`，`line_for_offset` 二分查行，供运行时 `ip -> 行号` 映射。`SourceFile*` 由拥有该 CodeUnit 的 `ObjFunction` 经其 `module_` 持有，`LineCol` 的列在运行时按需由 `SourceFile::locate` 重算（避免每偏移存全 `LineCol`）。

`ObjFunction` 已落地：持 `ObjString* name_`、`CodeUnit unit_`（值成员）、`ObjModule* module_`、`u8 arity_`；`ObjFunction::trace` 标 name、module、委托 `unit_.trace`（常量池；module_ 回指成环，mark-sweep 三色标记天然破环）。`Array<UpvalueDesc>` 捕获描述表（每条 `{is_local: bool, index: u16}`，见 §4.13）已落地（M4）：编译器 `compile_function` 尾部把子上下文登记的捕获描述一次性 flush 进来，VM 执行 `CLOSURE` 时只读。

## 8. 反汇编器输出格式（已落地）

输出含表头、常量池小节（非空才列）、code 段与结尾行：

```
== <main> ==

constants:
  0000: "hello"

code:
0000    1 LOAD_CONST    0000            ; "hello"
0003    | PRINT
0004    | LOAD_IMM      01              ; 1
0006    | JUMP_FALSE    -> 000B

== end ==
```

- 每行：偏移（4 hex）+ 行号（右对齐 4 列十进制，与上行同号用 `|` 占位）+ opcode 名（左对齐列宽）+ 操作数 hex + `;` 解析注释（常量值/名字/跳转目标/立即数/flags/argc，按操作数种类渲染）。
- 前向跳转目标用 `-> 偏移`、后向（`JUMP_BACK`）用 `<- 偏移` 直观显示。
- 注释为**解析注释**（把操作数解释成人可读形式），不含栈效应标注；栈效应见 §4 各表。
- 解码表驱动：`Disassembler` 查 `code.hpp` X 表生成物（`kOpCodeNames`/`kOpCodeFormats`）按格式分发（§2.1），新增指令零改动；VM 主循环自持 switch（热路径操作数读取内联于各 case，不查表），新增指令需 X 表 + VM case 两处同步。

## 9. 待决设计点汇总

| # | 议题 | 建议 | 备选 |
| :--- | :--- | :--- | :--- |
| 1 | `STORE_*` 留值 vs 弹值 | peek-store（留值） | pop-store + 旋转/临时槽 |
| 2 | 常量索引位宽 | `u16` 统一（暂不变；真超 65535 再加 `LOAD_CONST_L`） | `u8` + 长变体 |
| 3 | 跳转 / 局部槽位宽 | 跳转 `u16` + 方向拆分（前向 `JUMP*`/后向 `JUMP_BACK`）、局部 `u8` + `LOAD_LOCAL_L`/`STORE_LOCAL_L`(`u16`)（**已定**，§2.3/§4.12） | `i16`/`i32` 长变体 / 硬限报错 |
| 4 | `ADD` 重载 | 承载数值加 + 字符串/list 拼接 | 仅数值，拼接走内建 |
| 5 | `INVOKE_METHOD` | 预留指令: 编译器不发射, VM `not_implemented` | 维持 `LOAD_FIELD`+`CALL` |
| 6 | 异常机制 | CodeUnit 内记录表（M3 已落地; `finally` 已裁撤、后继 defer 已降为可选后续） | （已弃 `SETUP_EXCEPT`/`END_EXCEPT` 操作码方案） |
| 7 | `MAKE_RANGE` | 已加入 | -- |
| 8 | 整除/浮除语义 | **已定**：双 Int 整数除法（截断）、除零报 `DivisionByZero`；含 F64 走 IEEE | -- |
| 9 | 内建注册机制 | **已落地**: VM 级 builtins 表 + `LOAD_GLOBAL` 回退, 无新指令 | -- |
| 10 | CodeUnit 调试行信息 | 每偏移 `u32` 行号（RLE） | 存全 `LineCol` / 不存 |

## 10. 参考

- `src/bytecode/code.hpp`：`OpCode` 枚举（本文基准）。
- `docs/grammar.txt`：语言文法（lowering 的需求来源）。
- `.claude/reference/compile/compound-assignment-lowering.md`：复合赋值 / 前置 `++`/`--` 的 locator-once lowering（本文 §3.1/§5.3 以 peek-store 更新其 tail 语义）。
- `.claude/reference/memory/gc-implementation-plan.md` §5 Phase 3：CodeUnit / ObjFunction / ObjList / ObjMap 等子类型路线。
- `src/runtime/FrameStack.hpp`：`FrameStack<T,Capacity>` + `truncate(n)`，供异常 unwind。
- `src/value/Value.hpp` / `Value.cpp`：`value_hash`/`value_equal`/`value_identical`（`EQUAL`/`STRICT_EQUAL` 指令与全局表键语义来源；哈希键用 `===`）。
- CLAUDE.md「错误处理」第 2 条：VM 自管异常（`THROW` + CodeUnit 内异常记录表 `TryRecord`，不引入 `SETUP_EXCEPT`/`END_EXCEPT`，见 §4.16/§6.1）。
