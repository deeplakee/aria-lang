# 字节码指令集

栈式字节码 VM 的指令规格：功能 / 操作数位宽 / 栈效应 / 关键 lowering。指令编号和格式以 `src/bytecode/code.hpp` 的 `ARIA_OPCODE_LIST(X)` 为准。容器与编码方法见 [bytecode.md](../bytecode.md)，变更流程见 [aria-bytecode-change](../../skills/aria-bytecode-change/SKILL.md)。赋值类 lowering 见 [lowering.md](lowering.md)，异常通道见 [runtime.md](../runtime.md)，语言语义见 [文法](../../../docs/grammar.txt)。

`OpCode : u8`，共 79 条（含 16 条局部槽 N 短变体），`kOpCodeCount` 有测试哨兵；`OpFormat` 是操作数格式类别（`Simple`/`U8`/`U16`/`ConstU16`/`ImmI8`/`JumpFwd`/`JumpBack`/`RangeFlags`/`RegU8`/`Import`）。

## 1. 操作数编码

### 字节流

代码段是**字节流**：1 字节 opcode 后跟内联操作数，多字节操作数用小端。VM 在各 case 内直接读取操作数；反汇编器用 `kOpCodeFormats` 查表解码，两者遵循相同格式。

### 位宽

| 操作数种类 | 位宽 | 用于 | 理由 |
| :--- | :--- | :--- | :--- |
| 常量池索引 | `u16` | `LOAD_CONST` / 全局名 / 字段名 / `CLOSURE` / 类与方法名 / `IMPORT` | 模块级常量多，256 易超；统一 `u16` 免长短变体（不设 `u8` + `_L` 长变体，每条常量引用多 1 字节是接受的代价） |
| 局部槽号 | 零操作数（槽 1..8 内嵌枚举名）/ `u16` 通用 | `LOAD/STORE_LOCAL_1..8` + `LOAD/STORE_LOCAL` | 槽 1..8 覆盖绝大多数取局部（画像结论），1 字节指令消掉最热操作数；槽 0 与 >8 走通用形态 |
| Upvalue 索引 | `u8` | `LOAD/STORE_UPVALUE` | 256 远超实际；超限编译期 `TooManyUpvalues` |
| 调用参数数 | `u8` | `CALL` `CALL_METHOD` | 255 参数足够，超限编译期 `TooManyArguments` |
| 跳转偏移 | `u16` 无符号 | `JUMP*`（前向 `ip += off`）/ `JUMP_BACK`（后向 `ip -= off`） | 方向编码于 opcode，各得 64KB 量程；回边恒无条件、条件跳转恒前向，故无需有符号域与长变体 |
| `POP_N` 计数 | `u8` | `POP_N` | 单次 255 足够，更大由 `emit_pop_n` 分块 |
| 列表/映射元素数 | `u16` | `MAKE_LIST` `MAKE_MAP` | 字面量可能 >255；这俩不热，`u16` 免分批 lowering |
| 插值段数 | `u8` | `BUILD_STRING` | 单串 255 段（127 档）足够，超限编译期拒绝 |
| 立即整数 | `i8`（-128..127） | `LOAD_IMM` | 0/1/-1/小下标等高频小整数；大整数走 `LOAD_CONST` |
| 值寄存器索引 | `u8` | `LOAD_REG` | 寄存器分单例段 2 格 + 类段 12 格 = 14 格，`u8` 富余 |

### N 短变体与跳转方向拆分

**局部槽**：`LOAD/STORE_LOCAL` 是通用形态（`slot:u16`），槽 1..8 直接发零操作数短变体（槽号内嵌枚举名尾号，表内 8 行连续由 `code.hpp` static_assert 钉住，发射侧按枚举差换算、VM 侧逐 case 写死槽号）。槽号在分配时就确定，**发射即定形态、无需回填**。

**跳转**：偏移 `u16` 无符号、方向在 opcode--前向 `JUMP`/`JUMP_TRUE`/`JUMP_FALSE`/`JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP`/`JUMP_NE`，后向仅 `JUMP_BACK` 且恒无条件。依据：aria 只有 `while`/`for`/`for-in`（无 `do-while`），回边即循环体末尾跳回条件判断处；`if`/`while` 条件、`&&`/`||` 短路与 `match` 恒前向。前向跳转仍需回填（先占位、目标确定后写偏移），但只有一种宽度、无短/长选择。代价是单条跳转封顶 64KB，超限报 `CodeUnitTooLarge`。

## 2. 栈效应记号

- 栈横向写出、**右侧为栈顶**：`[a, b]` 表示 `a` 在下、`b` 在顶；`...` 表示更深栈内容。
- `[a, b] -> [c]`：弹 `a,b` 压 `c`，净 -1。

**STORE 系列统一 peek-store**：赋值是表达式、须产出值（`x = e` 的值即 `e`；`x += e`、`++x` 返回新值），故所有 `STORE_*` **只消费定位元、保留被存值于栈顶**：

| 指令 | 栈效应 |
| :--- | :--- |
| `STORE_LOCAL` / `STORE_UPVALUE` / `STORE_GLOBAL` / `STORE_THIS_FIELD` | `[v] -> [v]` |
| `STORE_FIELD` | `[obj, v] -> [v]` |
| `STORE_INDEX` | `[obj, idx, v] -> [v]` |

语句上下文（`exprStmt`）在 `STORE_*` 后补一条 `POP` 丢弃。故本文各 lowering 图末尾的「新值」就是赋值表达式的值。

## 3. 指令详表

每条给出操作数 / 栈效应 / 语义；`无` 表无操作数。

### 3.1 停机与空操作

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `HALT` | 无 | `... -> [..., nil]` | 停止主循环；断言不在协程内、压 nil 后返 true。编译器从不发射（顶层退出走 `RETURN`），仅供手写字节码 |
| `NOP` | 无 | `... -> ...` | 空操作（占位、调试断点） |

### 3.2 常量与字面量加载

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_CONST` | `idx:u16` | `[] -> [c]` | 压入常量池 `constants_[idx]`（串 / 大整数 / 浮点 / 函数原型） |
| `LOAD_IMM` | `n:i8` | `[] -> [n]` | 压入小整数立即数（避开常量池） |
| `LOAD_NIL` | 无 | `[] -> [nil]` | 压入 nil |
| `LOAD_TRUE` | 无 | `[] -> [true]` | 压入 true |
| `LOAD_FALSE` | 无 | `[] -> [false]` | 压入 false |
| `LOAD_REG` | `n:u8` | `[] -> [regs[n]]` | 压值寄存器 `n`（注册表见 `runtime/value_register.hpp`：单例段 2 格 DefaultMark/MatchNoArm + 类段 12 格；bootstrap 填充、tracer 逐格标根；**只读，无 STORE_REG**）。发射点 = 缺省参数印章、match 无臂哨兵、隐式超类 ObjectClass |

> NanBoxing 下 `Value{}` 零填充是 f64 `0.0` **非 nil**（见 `value.md`），`LOAD_NIL` 必须产出 `Value::nil_val()`。

### 3.3 局部变量

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_LOCAL` | `slot:u16` | `[] -> [v]` | 压入当前帧 `slots_[slot]`（槽 0 与 >8 用） |
| `STORE_LOCAL` | `slot:u16` | `[v] -> [v]` | peek-store |
| `LOAD_LOCAL_1..8` | 无 | `[] -> [v]` | 压入 `slots_[k]`，k 内嵌枚举名 |
| `STORE_LOCAL_1..8` | 无 | `[v] -> [v]` | peek-store 到 `slots_[k]` |

N 短变体与通用形态语义逐位一致，选择收口在 `CodeUnit::emit_load_local`/`emit_store_local`。

**局部槽分配与值填槽**：普通函数帧槽 0 为哑元（callee 占位），用户局部自槽 1 起、形参占槽 1..n；方法帧形 `[this, a1..aN]`，`this` 占槽 0。「槽区不会被自动保留」--临时值压栈恰好会写进 `slots[k]`，故编译器须保证**每个局部槽在作为活动局部使用前已被填充**。填充机制：声明局部时**不预占**（`define_local_or_fail`/`add_local` 只登记、不发指令），初始化器的值（或无初始化器时一条 `LOAD_NIL`）恰好压在该槽位即完成填充（依赖「declare 与 init 原子相邻、此前所有局部已填充」不变式，故「下个局部 slot = 当前栈高」，无需 store/pop）。for-in 的 `<iter>` 与 pattern 同用值填槽。

### 3.4 Upvalue 与闭包

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_UPVALUE` | `idx:u8` | `[] -> [v]` | 压入当前闭包第 `idx` 个 upvalue（开指栈槽或已关闭值） |
| `STORE_UPVALUE` | `idx:u8` | `[v] -> [v]` | peek-store（写穿外层槽或已关 cell，不查 init、不 mark） |
| `CLOSE_UPVALUE` | 无 | `[] -> []` | 批量关闭所有槽址 >= 当前栈顶的开 upvalue：值迁入 `ObjUpvalue.closed`，**不弹栈**（弹栈由前置指令完成） |

`CLOSE_UPVALUE` 的发射收口在 `CodeGen::emit_pop_locals_to`（退出作用域与 break/continue 共用）：先经 `emit_pop_n` 弹出整个区间（被捕获局部也计数），**弹区含被捕获局部才追加**一条 `CLOSE_UPVALUE`（弹栈和关闭之间无覆写、无分配、无 GC 安全点；新栈顶之上的开 upvalue 只属弹区局部）。落点包括 for-in 每轮出口（每轮关闭旧 upvalue，下一轮重新捕获）。RETURN / unwind 由 VM 关闭被丢弃区间的 upvalue，无需指令。

### 3.5 全局变量

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `DEF_GLOBAL` | `name:u16` | `[v] -> []` | 弹值，以 `constants_[name]` 为键写入当前模块 globals（命中覆写）；用于顶层 `var`/`fun`/`def`/import 别名 |
| `LOAD_GLOBAL` | `name:u16` | `[] -> [v]` | 查当前模块 globals，miss 回退 VM 级 builtins 表（8 个函数 `type`/`str`/`println`/`assert`/`clock`/`Error`/`is_a`/`is_subclass` + 12 个内建类 `Object`/`Exception`/`Iterator`/`List`/`Map`/`String`/`Range`/`Function`/`BoundMethod`/`Module`/`Coroutine`/`Class`），再 miss 报 `UndefinedVariable` |
| `STORE_GLOBAL` | `name:u16` | `[v] -> [v]` | peek-store 到模块 globals；未定义报 `UndefinedVariable`（赋值不隐式创建，必须先 `var`），**不回退 builtins** |

全局表是各 `ObjModule` 持有的 `AriaHashTable`（键为装箱的 intern `ObjString*`），`name` 操作数是常量池索引。builtins 表是构造期一次性填充的只读表，不引入 `LOAD_BUILTIN` 指令。

### 3.6 字段

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_FIELD` | `name:u16` | `[obj] -> [v]` | 弹接收者，压 `obj.name`；命中实例方法时压**绑定方法**（供后续 `CALL`），命中原值原样压 |
| `STORE_FIELD` | `name:u16` | `[obj, v] -> [v]` | peek-store：弹 `obj`、存 `obj.name = v`、留 `v` |

`obj.m(args)` 的**调用**不走 `LOAD_FIELD`（两段式见 §4.5）；`obj.m` 的**读取**仍经此指令现场绑定，**每次访问产出新 `ObjBoundMethod`**（不缓存：类/父类改写方法要对既有实例立即生效，`===` 因此不为真，内容相等走 `==`）。

`STORE_FIELD` 的接收者是 `ObjClass` 时即类上赋值（写遮蔽落接收类自身表）；类体内创建静态成员的镜像指令是 `MAKE_STATIC`，两者栈进同形、出口互反，故必须是两条指令。

### 3.7 下标

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_INDEX` | 无 | `[obj, idx] -> [v]` | 压入 `obj[idx]`（list/map/string；越界/类型不符为运行期错误） |
| `STORE_INDEX` | 无 | `[obj, idx, v] -> [v]` | peek-store |

栈顶为 `idx`、其下为 `obj`，无内联操作数。

### 3.8 this 字段

`this` 在方法帧固定占槽 0（方法帧 `[this, a1..aN]`）。`obj.field` 当 `obj` 恰为 `this` 时省去压 `this`：

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `LOAD_THIS_FIELD` | `name:u16` | `[] -> [v]` | 压 `this.name`（this 取帧槽 0，不压栈） |
| `STORE_THIS_FIELD` | `name:u16` | `[v] -> [v]` | peek-store 到 `this.name` |

纯 `this` 表达式：当前帧是实例方法 -> `LOAD_LOCAL 0`；嵌套函数 -> 捕获为 upvalue（`LOAD_UPVALUE`）。链上无实例方法时报编译期错误。

### 3.9 算术 / 比较 / 逻辑

二元 `[a, b] -> [r]`（净 -1），一元 `[a] -> [r]`（净 0），均无操作数。

| 操作码 | 栈效应 | 语义 |
| :--- | :--- | :--- |
| `EQUAL` | `[a, b] -> [a==b]` | 内容相等（`value_equal`：Int/F64 跨类型 IEEE 数值、Obj 调虚 `equals`；`-0.0 == 0.0` 真、`NaN != NaN`） |
| `NOT_EQUAL` | `[a, b] -> [a!=b]` | 取反 |
| `STRICT_EQUAL` | `[a, b] -> [a===b]` | 严格相等（`value_identical`：类型严格、f64 按位、Obj 指针；`1 === 1.0` 假、`-0.0 !== 0.0`、`NaN === NaN` 真） |
| `STRICT_NOT_EQUAL` | `[a, b] -> [a!==b]` | 取反 |
| `GREATER` / `GREATER_EQUAL` / `LESS` / `LESS_EQUAL` | `[a, b] -> [bool]` | 数值比较；类型不符为运行期错误 |
| `ADD` | `[a, b] -> [a+b]` | 数值加；对象左值经 `__add__` 钩子（String 拼接即内建钩子、用户类可重载，**仅左操作数触发**），钩子 miss 报 `UndefinedProperty`，非对象左值报 `TypeMismatch` |
| `SUBTRACT` / `MULTIPLY` | `[a, b] -> [r]` | 数值减 / 乘 |
| `DIVIDE` | `[a, b] -> [a/b]` | 双 Int 走整数除法（向零截断），除零报 `DivisionByZero`，`MIN / -1` 报 `IntegerOverflow`；含 F64 走 IEEE |
| `MOD` | `[a, b] -> [a%b]` | 整数余数按向零截断的商，`MIN % -1` 得 0，模零报 `ModuloByZero`；含 F64 走 fmod |
| `NOT` | `[a] -> [!a]` | 逻辑非（按真值翻转，结果 bool） |
| `NEGATE` | `[a] -> [-a]` | 数值取负（Int 的 MIN 报 `IntegerOverflow`）；对象经 `__neg__` 钩子 |

9 个数值二元算子（4 比较 + 5 算术）的对象左值全有钩子：`__lt__`/`__le__`/`__gt__`/`__ge__`/`__add__`/`__sub__`/`__mul__`/`__div__`/`__mod__`。整数加/减/乘/除与一元取负的结果须在 i48 域内，越界报 `IntegerOverflow`，不回绕、不自动升浮点；复合赋值与前置自增复用这些指令，算术失败不写回（已发生的操作数副作用保留）。

`&&`/`||` 短路，不设 `AND`/`OR`，经 `JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP` lowering（§4.2）。

### 3.10 栈操作

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `POP` | 无 | `[v] -> []` | 丢弃栈顶 |
| `POP_N` | `n:u8` | `[v1..vn] -> []` | 丢弃栈顶 `n` 个 |
| `DUP` | 无 | `[a] -> [a, a]` | 复制栈顶 1 槽 |
| `DUP2` | 无 | `[a, b] -> [a, b, a, b]` | 复制栈顶 2 槽（保序） |

`DUP`/`DUP2` 为复合赋值的 locator 保留而设（`lowering.md`），亦是通用栈操作。

### 3.11 控制流

偏移相对于**读完操作数后的下一字节**。

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `JUMP` | `off:u16` | `... -> ...` | 前向无条件 |
| `JUMP_TRUE` | `off:u16` | `[v] -> []` | 弹 `v`，真则前向跳 |
| `JUMP_FALSE` | `off:u16` | `[v] -> []` | 弹 `v`，假则前向跳 |
| `JUMP_TRUE_OR_POP` | `off:u16` | 真：`[v] -> [v]`（跳且留值）；假：`[v] -> []` | `||` 短路 |
| `JUMP_FALSE_OR_POP` | `off:u16` | 假：`[v] -> [v]`（跳且留值）；真：`[v] -> []` | `&&` 短路 |
| `JUMP_BACK` | `off:u16` | `... -> ...` | 后向无条件；while/for/for-in 回边、`continue`。回边兼 GC 安全点 |
| `JUMP_NE` | `off:u16` | `[a, b] -> []` | `EQUAL; JUMP_FALSE` 的融合形：`value_equal` 判等，不等则前向跳 |

`_OR_POP` 变体保证短路时被测值本身成为表达式结果（`a && b` 当 `a` 假时结果为 `a`），落空分支弹掉被测值使两路在汇合点栈深一致。`if`/`while` 条件用弹出版。

**融合只在 `==` 直连时做**：`JUMP_NE` 由 `CodeGen::emit_cond_jump` 在「条件表达式恰为 `==` 二元」时直发（`if`/`while`/`for` 条件、三元 `if` 表达式），`match` 臂判等与缺省参数印章判等两处自产 `EQUAL` 也按构造直发；判定在 **AST 形状**上做，不能看发射期尾字节--`||` 链 rhs 的 `==` 也以 `EQUAL` 结尾但那个值是链的中间结果，按尾字节改写会双弹操作数并把跳转占位覆写到已回填的链尾补丁上。`for-in` 的 `has_next` 调用结果不融合。

**`LESS; JUMP_FALSE` 有意不融合**：`LESS` 的对象臂可经 `__lt__` 钩子把控制流转进被调帧（进帧即返、被调体在下一轮 dispatch 才执行），「比较 + 按结果跳转」无法在单条指令的一个 case 内跨帧完成；数值/string 快路径虽可就地完成，但发射期无从预知接收者类型。故 `LESS` 尾保持两指令序列。

### 3.12 函数调用与闭包

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `CALL` | `argc:u8` | `[callee, a1..aN] -> [r]` | 调 `callee`。`ObjFunction` -> 按载荷调 Aria 函数或原生入口；`ObjClass` -> 实例化（分配 `ObjInstance` + 调 `init`，返实例）；`ObjBoundMethod` -> 用绑定接收者调方法；其余对象经 `__call__` 钩子取实现后以同一调用区递归分发；非对象报 `CallNonCallable` |
| `CLOSURE` | `proto_index:u16` | `[] -> [closure]` | 取常量池 `ObjFnProto`，建 `ObjFunction` 并按捕获描述表填 upvalue 数组，**立即压栈**（栈即根） |

`CALL` 兼作实例化（`Foo(args)` 编译为 `LOAD Foo` + 实参 + `CALL`），故无 `NEW` 指令。

**捕获描述存于 `ObjFnProto`**（`Array<UpvalueDesc>`，每条 `{is_local: bool, index: u16}`），由 `compile_function` 尾部一次性 flush，**不进字节码流**--故 `CLOSURE` 定长 3B、反汇编器线性扫即可。`is_local=true` 捕获外层帧局部槽 `index`（同槽经开链复用同一 `ObjUpvalue`，「捕获即引用」）；`false` 捕获外层闭包第 `index` 个 upvalue（穿透）。解析收口 `CodeGen::resolve_upvalue`（递归；容量超限报 `TooManyUpvalues`）。

### 3.13 类与对象

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `MAKE_CLASS` | `name:u16` | `[super] -> [class]` | 从栈顶读取 super，保留它作为分配时的 GC 根；建好 `ObjClass` 后覆写原槽。无显式父类时编译器先发 `LOAD_REG ObjectClass`；**内建容器五类（List/Map/String/Range/Iterator）不可作 super**（`cannot inherit from built-in class '<name>'`）、非类 super 报 `TypeMismatch`（均运行期错误） |
| `MAKE_METHOD` | `name:u16` | `[class, method] -> [class]` | 弹方法闭包、注册为实例方法（**仅实例方法、仅收 Aria 函数**）；戳 `defining_class`（一职双任：super 来源 + 方法性标记）。命中 `"init"` 时经 `set_field` 同步 `ObjClass.init_`；`class` 留栈继续接收成员 |
| `MAKE_STATIC` | `name:u16` | `[class, value] -> [class]` | 弹值存为静态变量或静态方法；**不戳** `defining_class` ⟹ 静态槽持函数值读恒原值 |
| `LOAD_SUPER_FIELD` | `name:u16` | `[] -> [v]` | `this` 取帧槽 0，父类取当前闭包的 `defining_class->superclass`，沿父链查 `name`；带方法戳的闭包绑成 `ObjBoundMethod`，其余原值直读 |
| `PREPARE_METHOD` | `name:u16` | `[recv] -> [recv, target]` | 两段式第一段：接收者在栈顶（实参尚未求值），经 `Object::load_field` 取被调值压栈 |
| `CALL_METHOD` | `argc:u8` | `[recv, target, a1..aN] -> [r]` | 两段式第二段：待调值在 `peek(argc)`、接收者在 `peek(argc+1)`；实参整体下移一格补掉 target 那格得 `[recv, a1..aN]`，交 `call_value`。**纯调用、不再解析、不物化 bound** |
| `MAKE_LIST` | `n:u16` | `[v1..vn] -> [list]` | 取栈顶 n 个建 `ObjList`（保序） |
| `MAKE_MAP` | `n:u16` | `[k1,v1..kn,vn] -> [map]` | 取栈顶 n 对建 `ObjMap`（重复键后键胜） |
| `MAKE_RANGE` | `flags:u8` | `[from, to] -> [range]` / `[from] -> [range]` | 建 `ObjRange`（内容哈希、不可变）；`flags` 位义见 `code.hpp`：`0x00` 含上界、`0x01` 不含、`0x02` 无上界（`from..` 与 `from...` 同义）。端点非整数报 `TypeMismatch` |
| `BUILD_STRING` | `n:u8` | `[v1..vn] -> [str]` | 取栈顶 n 段按序拼成单串（插值收拢）；渲染与 `str()` 同源（`stringify`，String 段直写字节零渲染），量长定容单次分配 |

**def 声明的发射序**（文法见 `grammar.txt`，成员语义见 `object.md`）：

```
<super>                    ; 求值显式父类表达式 / 无父类 LOAD_REG ObjectClass
MAKE_CLASS "Foo"           ; [class]
# 绑定类名紧跟 MAKE_CLASS（不等到成员发完）：
#   顶层  DUP; DEF_GLOBAL "Foo"
#   局部  直接将当前槽登记为局部（零发射）
#   嵌套  DUP2; MAKE_STATIC "B"; POP     -- A.B 作为 A 的静态成员
# 然后按成员出现顺序发射，class 始终留栈：
<expr>  MAKE_STATIC "x"    ; 静态变量（eager 求值初始化器）
CLOSURE MAKE_STATIC "s"    ; 静态方法（fun）
CLOSURE MAKE_METHOD "m"    ; 实例方法（含 init）
POP                        ; 只在顶层或嵌套类成员路径发射；局部类值保留到作用域退出
```

类名先绑定使类体内可自引用（`A.x`、构造 `A()`）；静态初始化器 throw 后类名保持绑定（残留语义）。

**`super.m(args)` 不经两段式**（callee 是 `SuperExprNode`）：`LOAD_SUPER_FIELD` + 实参 + `CALL`。

### 3.14 模块导入

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `IMPORT` | `path:u16` | `... -> [module]` | 解析路径并复用/加载模块，把 `ObjModule` 压栈 |

`path` 是常量池 ObjString 索引。**绑定不在本指令内**：CodeGen 在 `IMPORT` 之后按作用域声明并绑定（顶层 `DEF_GLOBAL alias` / 嵌套值填槽），故 `IMPORT` 不带 `alias` 操作数。解析、查表、run-once 与错误契约见 `import.md`。

### 3.15 异常

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `THROW` | 无 | `[v] ->` | 弹 aria 值作异常抛出（**原值**入挂起寄存器，不包 `ObjException`，catch 绑原值保类型）；unwind 查各帧 `try_records` 回退到最近 handler，栈由 unwind 重建 |

**异常走 CodeUnit 内记录表，不设 `SETUP_EXCEPT`/`END_EXCEPT`**：`try` 范围与 handler 地址由编译期生成的 `Array<TryRecord>` 登记（`{begin, end, handle, stack_depth}`，按 `begin` 非降序、由入口预插占位 + 结尾回填保证），`THROW`/`raise` 按帧 `last_ip` 反推 offset 二分查表（区间须良嵌套）。比操作码方案更紧凑：不污染字节码流、无需进/出 try 发指令。

### 3.16 返回

| 操作码 | 操作数 | 栈效应 | 语义 |
| :--- | :--- | :--- | :--- |
| `RETURN` | 无 | `[v] ->` | 从当前函数返回 `v`（无返回值时编译器先发 `LOAD_NIL`）。退帧、把 `v` 写回调用者槽 0、`ip` 恢复 |

入口函数（`<main>` 与导入模块体 `<module>` 同为 ModuleEntry 形态）的返回值恒为模块对象--编译器在入口收尾发射「压模块对象常量 + RETURN」，裸 `return;` 同形，带值 `return` 编译期拒绝（`ReturnValueAtTopLevel`），故 `IMPORT` 命中/未命中栈效应统一 `[..., module]`。

## 4. 关键 lowering

### 4.1 if / while / for / for-in

```
# if / while / for 的条件经 emit_cond_jump：条件恰为 == 直连时发 JUMP_NE，否则 EQUAL/值 + JUMP_FALSE
<cond>                 ; [c]
JUMP_FALSE L_else      ; []            （== 直连时即 JUMP_NE L_else）
<then>
JUMP L_end
L_else: <elseB>
L_end:

# while (cond) body
L_start: <cond>; JUMP_FALSE L_end; <body>; JUMP_BACK L_start; L_end:
# for (init; cond; incr) body
<init>; L_cond: <cond>; JUMP_FALSE L_end; <body>; L_incr: <incr> + POP; JUMP_BACK L_cond; L_end:
# break -> JUMP L_end；continue -> 有 incr 时前向 JUMP 回填到 L_incr，否则 JUMP_BACK 循环头
```

break/continue 在跳转前先 `emit_pop_locals_to(loop_scope_depth)` 弹体内局部；回填时机与 `LoopCtx` 全貌见 `lowering.md`。

for-in 依赖迭代协议，三站点都发两段式调用：

```
<iterable>
PREPARE_METHOD "iter"; CALL_METHOD 0          ; [it]    迭代器存 for-in scope 局部（值填槽）
L_start:
LOAD_LOCAL <iter>; PREPARE_METHOD "has_next"; CALL_METHOD 0
JUMP_FALSE L_end
LOAD_LOCAL <iter>; PREPARE_METHOD "next"; CALL_METHOD 0
<绑定 pattern>                                  ; Fill：标识符值填槽 / 解构 / "_" 弹掉结果
<body>
<清理本轮局部>                                  ; 含捕获时追加 CLOSE_UPVALUE
JUMP_BACK L_start
L_end:
POP                                             ; 退出整个循环，弹隐藏局部 <iter>
```

### 4.2 短路逻辑

```
# a && b（a 假则结果为 a，否则为 b）
<a>; JUMP_FALSE_OR_POP L_end; <b>; L_end:
# a || b（a 真则结果为 a，否则为 b）
<a>; JUMP_TRUE_OR_POP L_end; <b>; L_end:
```

### 4.3 赋值 / 复合赋值 / 前置 `++`/`--`

locator-once 的完整方案（`LvalueMode` 四态、三种左值腿、组合矩阵、逐行示例）见 `lowering.md`。此处只记两条：store 腿是 peek-store（§2），语句上下文补 `POP`；`x += e` / `++x` 的结果是新值。

### 4.4 闭包 / upvalue

```
CLOSURE proto_index    ; [closure]   捕获描述在原型元数据里，无尾随操作数
# 块结束且弹区含被捕获局部：先弹局部，再 CLOSE_UPVALUE（emit_pop_locals_to，见 §3.4）
```

### 4.5 方法调用（两段式）

```
<recv>
PREPARE_METHOD "m"     ; [recv, target]   解析此刻完成（实参尚未求值）
<args>                 ; [recv, target, a1..aN]
CALL_METHOD argc       ; [r]              实参下移一格补掉 target
```

- **次序**：解析先于实参求值（Python/Lua/JS 同款）。可观察两处：实参表达式反过来改写接收者同名成员时，本次调用用**改写前**取好的值；解析失败时实参**根本不跑**。语料 `positive/11_classes/invoke_method_forms.aria` 钉住。
- **查询与绑定**：`PREPARE_METHOD` 经 `Object::load_field` 取原值，不创建绑定方法；实例字段仍优先于类链。单独读 `recv.m` 则经 `load_field_bound`，按宿主的成员规则决定是否绑定（见 [对象参考](../object.md)）。
- **槽 0**：`CALL_METHOD` 整理调用区后，槽 0 初始是点号左侧接收者。脚本或原生函数直接沿用该槽；已有 `ObjBoundMethod` 用自身绑定的接收者覆写；类调用与 `__call__` 钩子各自设置槽 0（见 [运行时参考](../runtime.md)）。
- **可观察差异**：`C.m()` 经类调用实例方法时，`this` 是类对象；`var f = C.m; f()` 中类上读取返回原函数，普通 `CALL` 的槽 0 是该函数值。`x.init()` 若命中 Object 的原生 no-op `init`，它不写返回槽，因此返回 `x`。

### 4.6 match

无独立指令：subject 求值一次驻留栈上（in-flight 临时、无隐藏局部），逐臂展开「`DUP` + 模式表达式 + `JUMP_NE` 下臂」链：

```
<subject>              ; [s]
DUP; <p1>; JUMP_NE L_arm2   ; [s]  判等不等则跳下臂（融合形，见 §3.11）
POP                          ; []   命中：丢 subject 进臂体
<body1>
JUMP L_end
L_arm2:                # "_" 臂：无比较，POP 直入臂体
POP; <body2>; JUMP L_end
L_noarm:               # 全臂未命中
LOAD_REG MatchNoArm    ; [e]  共享 ObjException（bootstrap 铸造，消息静态）
THROW
L_end:
```

- `matchStmt` 与 `matchExpr` 同构：臂体分别为语句（净零值）/ 单表达式（每臂恰一值），`L_end` 汇合点栈深各自还原（语句 0 / 表达式 +1）。
- **臂间分隔符不同**：`matchStmt` 臂间无分隔符；`matchExpr` 臂间以 `,` 分隔、不允许尾逗号（见 `grammar.txt`）。
- 通配臂恒末臂且至多一条（其后臂不可达，编译期 `UnreachableArm`）；`_` 末臂在场时兜底尾为死码仍统一发射。pattern 按臂序惰性求值（命中即停）。

### 4.7 解构赋值

源值恒驻栈顶，每次访问前 `DUP`（`LOAD_INDEX` 吃掉源与下标两值）；绑完收尾弹掉本层源值，净消耗栈顶一值，故调用点先 `DUP` 一份右值作表达式结果。

```
# [a, _, c] = rhs
<rhs>; DUP                  ; [rhs, rhs]  右值（表达式值由调用点先 DUP 一份）
DUP; LOAD_IMM 0; LOAD_INDEX  ; [rhs, rhs, rhs[0]]
STORE_...; POP              ; [rhs, rhs]  写 a，丢弃 peek-store 保留的值
DUP; LOAD_IMM 2; LOAD_INDEX  ; [rhs, rhs, rhs[2]]
STORE_...; POP              ; [rhs, rhs]  写 c
POP                         ; [rhs]       只弹本层源值
```

`"_"` 位不产生下标访问，故该位置越界/缺键都不报。上图是给已有名字赋值的 `Store` 路径。声明与 for-in 使用 `Fill`：0 次访问直接弹源值，1 次访问消耗源值，至少 2 次访问时把源值放进隐藏局部并逐次重读。**rest 位** `...rest`：压前缀位置数后 `MAKE_RANGE`（无上界位）作键取后缀，即无上界切片语义（空尾与空源都给空 list），无专用指令。

### 4.8 try / catch

编译期为每个 `try` 在 CodeUnit 的记录表生成一条 `{begin, end, handle=L_catch, stack_depth}`；字节码本身不含 try 设置/清理指令，`<body>` 直接顺序排列：

```
<body>                 ; try 体内顺序执行
JUMP L_end             ; 正常完成，跳过 catch
L_catch:               ; 抛出时 unwind 到此；截栈后 push 异常值，恰落 catch 参数槽（== stack_depth，值填槽、无 STORE_LOCAL）
  <catch_body>
L_end:
```

`Eval::unwind(vm)`：按帧 `last_ip` 反推 offset、自最内帧向外查记录表（纯搜索，不动帧栈/值栈、不物化）-> 命中帧调 `AriaExeContext::unwind_to_handler(帧索引, record)` 一体完成（弃内层帧 + 按槽址关开 upvalue + 栈顶截到 catch 槽 + `ip = handle` + 载荷落槽）；全帧未命中按出口边界三分流（嵌套执行截净残帧续搜 / 协程链转投一跳 / 主上下文物化）。物化归 `AriaVM::rebake_uncaught_error`，跟踪行逐帧烘焙在消息尾部。`finally` 不支持（无 finally 汇合点；善后 defer 已裁定不做）。

## 5. 相关文档

- `bytecode.md`：`CodeUnit` 容器、emit/回填方法、反汇编器输出格式。
- `lowering.md`：赋值类 locator-once（`LvalueMode`、三种左值腿、`LoopCtx` 与 break/continue 回填）。
- `import.md`：模块解析与加载。`runtime.md`：异常通道、`CallFrame`、执行入口。
- `docs/grammar.txt`：文法与作用域模型（本文只讲发射，不复述语义）。
