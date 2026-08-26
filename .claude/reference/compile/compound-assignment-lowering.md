# 复合赋值的字节码 lowering

aria 是栈式字节码 VM。复合赋值 `E1 op= E2`(op ∈ {+,-,*,/,%})相对普通赋值的核心语义约束是 **左值定位(locator)只求值一次**--C 标准 `E1 += E2` 定义:等价 `E1 = E1 op E2`,但 `E1` 只求值一次。本文档记录该约束的字节码 lowering 方案,供 VM/编译器实现参考。前置自增自减 `++E`/`--E` 同属此列,见 §5。

文法层的不变式见 `docs/grammar.txt` 说明区;本文是其编译期实现方案。

## 1. 问题:为什么要专门处理

复合赋值不是普通赋值的字面语法糖。naive 把 `arr[f()] += 1` 展成 `arr[f()] = arr[f()] + 1` 再按普通赋值编译,会让 locator(此处 `arr` + `f()`)求值两次:`f()` 调两次,结果错误(副作用重复,且浪费)。

读和写本身是两次操作(先读旧值、算新值、再写回),不可避免;要避免的只是"决定写到哪"的子表达式跑多次。故问题归约为:**把"求值左值的位置(locator)"与"通过位置 load/store"分开,位置只算一次、load 和 store 共用。**

## 2. 左值形式与 locator 构成

文法把合法左值限定为 `identifier / obj.field / obj[index]`(合法性留语义阶段校验)。locator 的运行时部分:

| 左值形式 | locator 运行时部分 | 是否需 DUP |
| :--- | :--- | :--- |
| `identifier`(局部 / upvalue / global) | 编译期常量(槽位号 / upvalue 索引 / 全局名),**无运行时求值** | 否 |
| `obj.field` | `obj`(field 名是编译期常量) | `DUP` |
| `obj[idx]` | `obj` + `idx`(都可能带副作用) | `DUP2` |

## 3. 新增栈操作码

`OpCode`(`src/bytecode/code.hpp`)新增两个**无操作数**的栈操作码:

| 操作码 | 栈效果 | 说明 |
| :--- | :--- | :--- |
| `DUP`  | `[a] -> [a, a]` | 复制栈顶 1 个槽 |
| `DUP2` | `[a, b] -> [a, b, a, b]` | 复制栈顶 2 个槽(保序),对应 Python `DUP_TOP_TWO` |

> 两者复制数量固定编入操作码名、不带操作数,区别于 `POP_N`(带操作数 N)。引入主因是复合赋值 lowering,但属通用栈操作,其它"需保留栈顶供后续复用"的场景也可用。

## 4. lowering 序列

记 `<X>` 为"求值表达式 X 压栈"的子序列,`<op>` 为对应算术操作码(ADD/SUBTRACT/MULTIPLY/DIVIDE/MOD)。各左值形式的 `lhs op= e`:

### 4.1 标识符 `x op= e`(局部 / upvalue / global)

locator 是编译期常量,无运行时求值,最简单,无需 DUP。仅 load/store 操作码随变量种类不同:
```
LOAD_LOCAL x     ; [x]        (upvalue: LOAD_UPVALUE;global: LOAD_GLOBAL)
<e>              ; [x, e]
<op>             ; [x op e]
STORE_LOCAL x    ; []         (upvalue: STORE_UPVALUE;global: STORE_GLOBAL)
```

### 4.2 字段 `obj.f op= e`

`DUP` 保 `obj` 跨过 load 供 store 复用:
```
<obj>            ; [obj]
DUP              ; [obj, obj]
LOAD_FIELD "f"   ; [obj, obj.f]
<e>              ; [obj, obj.f, e]
<op>             ; [obj, obj.f op e]
STORE_FIELD "f"  ; []
```

### 4.3 下标 `obj[idx] op= e`

`DUP2` 保 `(obj, idx)` 对跨过 load 供 store 复用。`idx` 内的副作用(如 `f()`)只发生一次:
```
<obj>            ; [obj]
<idx>            ; [obj, idx]            <- 副作用在此,仅一次
DUP2             ; [obj, idx, obj, idx]
LOAD_INDEX       ; [obj, idx, obj[idx]]  消耗顶层一对,留下复制对
<e>              ; [obj, idx, obj[idx], e]
<op>             ; [obj, idx, obj[idx] op e]
STORE_INDEX      ; []    消耗留下的 (obj,idx) 与新值
```

> `LOAD_INDEX`/`STORE_INDEX` 的栈操作数顺序以 VM 实现为准;上表假设两者都从栈顶取 `(obj, idx)` 对、`STORE_INDEX` 额外取其下方的值。实现时若顺序不同,`DUP2` 后的栈布局相应调整,但"locator 只求值一次"不变。

## 5. 前置自增自减 (`++`/`--`)

`++E` / `--E` 与复合赋值同属 lvalue-once 操作:C 标准明文 `++E` 等价于 `E += 1`、`--E` 等价于 `E -= 1`(C11 §6.5.3.1p2/p3),故继承同一条"左值只求值一次"不变式。`++obj[f()]` 的 `f()` 只调一次,`++getObj().field` 的 `getObj()` 只调一次--与 `obj[f()] += 1` 完全相同。

lowering 复用 §4 的 DUP/DUP2 locator 保留手法,差别仅两点:运算对象是常量 1(`LOAD_IMM 1`)而非任意 rhs;前置 `++`/`--` 作为表达式**返回新值**。以 `++obj[idx]` 为例:

```
<obj>            ; [obj]
<idx>            ; [obj, idx]            <- 副作用在此,仅一次
DUP2             ; [obj, idx, obj, idx]
LOAD_INDEX       ; [obj, idx, obj[idx]]  消耗顶层一对,留下复制对
LOAD_IMM 1       ; [obj, idx, obj[idx], 1]
ADD              ; [obj, idx, newval]    newval = obj[idx]+1
STORE_INDEX      ; [newval]              存回 obj[idx]=newval(复用 (obj,idx)),留 newval 作返回值
```

`--E` 把 `ADD` 换 `SUBTRACT`。`++x`(标识符)/`++x.f`(字段)同理,按 §4.1/§4.2 无 DUP 或用 `DUP`。

> **返回新值的 tail**:前置 `++`/`--` 返回新值,与 `x += e` 返回新值同属"赋值类表达式返回什么值"的语义。已采纳 peek-store:`STORE_LOCAL`/`STORE_GLOBAL` 存后留值,`x = e` / `x += e` / `++x` 三者统一;语句上下文(表达式值被丢弃)补一条 `POP`。locator-once 不受此 tail 影响。

> aria 文法 `unary -> ( ... | "++" | "--" ) unary`,`value` 层无后置 `++`/`--`,故只有前置。若日后加后置 `x++`(返回旧值),仍 locator-once,但要先读出旧值保住、再自增存回(多一步保存旧值);locator 依旧只求值一次。

## 6. 编译器设计

字节码编译器(CodeGen)把"左值编译为 locator"与"编译为值"分开:

- `compile_lvalue(target)`:求值 target 的 locator(局部槽 / upvalue 索引 / 全局名 / `obj` 引用 + field / `obj` 引用 + idx),返回 locator 描述;**不**产生 load。
- 普通 `lhs = e`:`<e>` 压值 -> `compile_store(locator)`。
- 复合 `lhs op= e`:`compile_lvalue(lhs)` 一次 -> 经该 locator load -> `<e>` -> `<op>` -> 经**同一** locator store。

即 compound 复用一次 `compile_lvalue` 的结果做 load 与 store,这正是"只求值一次"在编译器里的落点,也是 `+=` 等视为"赋值类运算符同族"在实现上的体现。

## 7. 不要这么做

- **别在 AST 层 naive 重写** `E1 op= E2` -> `E1 = E1 op E2` 再按普通赋值编译--等于把 locator 求值两次的责任硬塞回去。
- **别指望 CSE/优化器消重**:CSE 只能消纯表达式;`f()` 有副作用,CSE 不能动它。脚本 VM 一般也无 CSE pass。这是**编译期 emission 的责任**,不是优化器的事。

## 8. 链式复合赋值

`a op= b op= c` 右结合解析为 `a op= (b op= c)`(见 `docs/grammar.txt`)。每个 compound 各自保证自己 locator 只求值一次;内层 `b op= c` 作为外层 rhs 求值(其值为赋值后的新 `b`)。链式不引入新的重复求值问题,沿用上述 lowering 即可。

## 9. 备选方案(未采纳)

locator 溢出到临时 local 槽:用现有操作码(`STORE_LOCAL`/`LOAD_LOCAL` 到编译器分配的临时槽)代替 `DUP`/`DUP2`。优点是无需新操作码;缺点是 ops 更多、需分配并管理临时槽生命周期。本设计选 `DUP`/`DUP2`:ops 少、无临时槽、与 Python 成熟方案一致。
