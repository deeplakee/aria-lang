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

`DUP` 保 `obj` 跨过 load 供 store 复用（`STORE_FIELD` 为 peek-store：`[obj, v] -> [v]` 弹 obj 留 v，赋值表达式留新值）：
```
<obj>            ; [obj]
DUP              ; [obj, obj]
LOAD_FIELD "f"   ; [obj, obj.f]
<e>              ; [obj, obj.f, e]
<op>             ; [obj, obj.f op e]
STORE_FIELD "f"  ; [obj.f op e]    消耗 (obj, 新值)，留新值
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
STORE_INDEX      ; [newval]    消耗留下的 (obj,idx) 与新值,留新值(peek-store 同族)
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

字节码编译器(CodeGen)把 load/store 的发射收归到访问节点,经上下文 flag `LvalueMode{Load,Store,Locate}`(CodeGen 成员 `lvalue_mode_`,默认 `Load`)告诉目标节点当前作为 load 还是 store:

- `emit_lvalue(node, mode)`:`validate_lvalue_target(node)` 后设置 `lvalue_mode_`、`node->accept(*this)` 分派(不在分派后恢复)。目标节点入口经 `take_lvalue_mode()` 一次性 take(取值并清空为 `Load`),故子节点经 `emit_expr` 时 flag 已清空、不泄漏。`emit_expr` 入口 `ASSERT(lvalue_mode_ == Load)` 开发期捕获漏 take 的 bug。
- `validate_lvalue_target(target)`(dynamic_cast 守卫):Identifier/Field/Index 三种合法左值种类放行(三种均已落地;解构赋值等其余形态非左值目标,落 `InvalidAssignmentTarget`)、其余 -> `InvalidAssignmentTarget`;由 `emit_lvalue` 在分派前调用(两种赋值首腿--普通 = 的 `Prepare`/复合与前置自增自减的 `Locate`--均先于 rhs 抛错,字节码随 throw 丢弃)。
- 两个 take 点(`visitIdentifierNode` / `visitFieldAccessNode`)按模式分派,全组合速查(其余节点经 `emit_expr` 时 flag 已清空为 `Load`)。记号:`resolve`/`name_idx`/init 登记均为编译期动作零指令;`<X>` = 发射子表达式 X(值压栈);peek-store = 读栈顶写回存储但不弹,值留栈作赋值表达式值;Prepare = 定位准备腿(只发接收者不读值,普通 = 首腿);「同形 Load」= 编译期常量 locator 的定位腿在节点 switch 内折叠为 Load 形(无运行时副本可留,见行内注):

| 目标节点·形态 | 模式 | 发射的指令 | 栈变化 |
| :--- | :--- | :--- | :--- |
| Identifier·任意 | Prepare | no-op(locator 编译期常量,无接收者可备) | -- |
| Identifier·局部 | Load | `LOAD_LOCAL slot` | [] -> [v] |
| Identifier·局部 | Store | `STORE_LOCAL slot` | [v] -> [v](peek-store) |
| Identifier·upvalue | Load | `LOAD_UPVALUE idx` | [] -> [v] |
| Identifier·upvalue | Store | `STORE_UPVALUE idx`(写穿外层槽/已关 cell;不查 init、不 mark) | [v] -> [v](peek-store) |
| Identifier·全局 | Load | `LOAD_GLOBAL name_idx` | [] -> [v] |
| Identifier·全局 | Store | `STORE_GLOBAL name_idx` | [v] -> [v](peek-store) |
| Identifier·任意 | Locate | 与 Load 同形(resolve + 同一 LOAD_*;locator = 槽位/捕获索引/全局名,编译期常量重解析免费) | [] -> [v] |
| FieldAccess·帧内 this | Prepare | no-op(this 取帧槽 0 不经栈,无接收者可备) | -- |
| FieldAccess·帧内 this | Load | `LOAD_THIS_FIELD name_idx`(this 取帧槽 0 不经栈) | [] -> [v] |
| FieldAccess·帧内 this | Store | `STORE_THIS_FIELD name_idx`(字段不存在动态创建) | [v] -> [v](peek-store) |
| FieldAccess·帧内 this | Locate | 与 Load 同形(`LOAD_THIS_FIELD`;写腿不经栈取 this,发 DUP 副本反而滞留无人消费) | [] -> [v] |
| FieldAccess·一般对象/捕获 this | Prepare | 只发 `<object>`(普通 = 首腿;不进名字池、不读值) | [] -> [obj] |
| FieldAccess·一般对象/捕获 this | Load | `<object>` + `LOAD_FIELD name_idx` | [] -> [obj] -> [obj.x] |
| FieldAccess·一般对象/捕获 this | Store | 只发 `STORE_FIELD name_idx`(接收者已由 Prepare 腿压在值下) | [obj, v] -> [v](弹接收者留新值) |
| FieldAccess·一般对象/捕获 this | Locate | `<object>` + `DUP` + `LOAD_FIELD name_idx`(副本垫底,供本节点 Store 腿复用) | [] -> [obj] -> [obj, obj] -> [obj, obj.x] |
| IndexAccess | Prepare | 只发 `<obj>` `<idx>`(为 Store 腿备对) | [] -> [obj] -> [obj, idx] |
| IndexAccess | Load | `<obj>` + `<idx>` + `LOAD_INDEX` | [obj, idx] -> [obj, idx, obj[idx]] |
| IndexAccess | Store | 只发 `STORE_INDEX`(obj/idx 已由 Prepare 腿备好) | [obj, idx, v] -> [v](peek-store) |
| IndexAccess | Locate | `<obj>` + `<idx>` + `DUP2` + `LOAD_INDEX`(复制对垫底) | [obj, idx] -> [obj, idx, obj, idx] -> [obj, idx, obj[idx]] |

  `super.成员` 不经 FieldAccess(独立 `SuperExprNode`,Load-only,写形态被 validate 拒绝)。生产侧三形对称,只差首腿与中间腿:普通 `=` = Prepare -> `<e>` -> Store;复合 `op=` = Locate -> `<e>` -> op -> Store;`++`/`--` = Locate -> `LOAD_IMM 1` -> op -> Store。

- 三腿拼装逐行示例,右注标出该行由哪条腿产出(模式只作用于 `emit_lvalue` 分派的目标节点;`<e>` 值腿走 `emit_expr` 恒 Load 语境,运算指令无模式;编译期常量 locator 的 Locate 腿在节点 switch 内折叠为 Load 同形):

`obj.f += e`(一般对象,Locate 腿的 DUP 副本由 Store 腿消费):
```
<object>            ; [obj]              Locate 腿(Locate 分派)发 object 子表达式
DUP                 ; [obj, obj]         Locate 腿:副本垫底
LOAD_FIELD f        ; [obj, obj.f]       Locate 腿:读旧值
<e>                 ; [obj, obj.f, e]    值腿(emit_expr,恒 Load 语境)
ADD                 ; [obj, new]         运算(无模式)
STORE_FIELD f       ; [new]              Store 腿(Store 分派):弹 (副本, 新值),留新值 = 赋值表达式的值
```

`this.x += e`(帧内 this,定位腿折叠 Load 形,全程无 DUP):
```
LOAD_THIS_FIELD x   ; [old]              Locate 腿(Locate 分派,帧内 this 折叠 THIS_FIELD Load 形)
<e>                 ; [old, e]           值腿(emit_expr,恒 Load 语境)
ADD                 ; [new]              运算(无模式)
STORE_THIS_FIELD x  ; [new]              Store 腿(Store 分派,THIS_FIELD Store 形):peek-store,this 取帧槽 0
```

`x += e`(Identifier,定位腿折叠 Load 形):
```
LOAD_LOCAL x        ; [old]              Locate 腿(Locate 分派,折叠同 Load)
<e>                 ; [old, e]           值腿(emit_expr,恒 Load 语境)
ADD                 ; [new]              运算(无模式)
STORE_LOCAL x       ; [new]              Store 腿(Store 分派):peek-store
```

`++obj.f`(前置自增,留新值即表达式值;语句上下文值被丢弃补一条 POP):
```
<object>            ; [obj]              Locate 腿(Locate 分派)发 object 子表达式
DUP                 ; [obj, obj]         Locate 腿:副本垫底
LOAD_FIELD f        ; [obj, obj.f]       Locate 腿:读旧值
LOAD_IMM 1          ; [obj, obj.f, 1]    常量腿(++ 固定压 1)
ADD                 ; [obj, new]         运算(无模式)
STORE_FIELD f       ; [new]              Store 腿(Store 分派):弹 (副本, 新值),留新值
```

普通 `obj.f = e`(Prepare 首腿由节点发接收者;Store 腿只发 STORE_FIELD):
```
<object>            ; [obj]              Prepare 腿(Prepare 分派):只发接收者
<e>                 ; [obj, e]           值腿(emit_expr,恒 Load 语境)
STORE_FIELD f       ; [e]                Store 腿(Store 分派):只发这一条
```
- 普通 `lhs = e`：`emit_lvalue(lhs, Prepare)` -> `<e>` -> `emit_lvalue(lhs, Store)`(peek-store)。Prepare = 定位准备腿，只发 locator 的运行时部分（一般对象/捕获 this 的 `<obj>`；Identifier/帧内 this 的 locator 编译期常量，臂恒 no-op）--`STORE_FIELD` 栈形 `[obj, v] -> [v]` 要求接收者先于值入栈，接收者发射权归目标节点；帧内 this 的写腿 `STORE_THIS_FIELD` 不经栈取 this。与复合赋值只差中间腿（值 vs op）。
- 复合 `lhs op= e`：`emit_lvalue(lhs, Locate)` -> `<e>` -> `<op>` -> `emit_lvalue(lhs, Store)`。生产侧统一发 `Locate`;目标节点按 locator 性质分派:编译期常量(Identifier;帧内 this 的 FieldAccess)定位腿与 `Load` 同形--重 resolve 廉价且无副作用,"locator 只求值一次"自然成立;运行时 locator(Field/Index)以 `Locate` 单次求值、经 `DUP`/`DUP2` 留 VM 栈、Load/Store 复用栈上副本(见 §4.2/§4.3),非编译期 stash。前置 `++`/`--` 同族,首腿同为 `Locate`(§5)。

即 locator-once 按 locator 性质两路实现:编译期常量(Identifier、帧内 this)走"重 resolve 廉价可重做",定位腿与 `Load` 同形(无副本可留);运行时 locator(Field/Index)走"VM 栈 DUP/DUP2 复用";两者都不靠编译期缓存 locator 描述。`Locate` 已随 M5 类落地启用:一般对象定位腿发 `<obj> DUP LOAD_FIELD`,帧内 this 定位腿折叠 `LOAD_THIS_FIELD`(写 `STORE_THIS_FIELD` 不经栈取 this);IndexAccess 的 DUP2 形态（§4.3）已随 list 下标接线落地。

## 7. 不要这么做

- **别在 AST 层 naive 重写** `E1 op= E2` -> `E1 = E1 op E2` 再按普通赋值编译--等于把 locator 求值两次的责任硬塞回去。
- **别指望 CSE/优化器消重**:CSE 只能消纯表达式;`f()` 有副作用,CSE 不能动它。脚本 VM 一般也无 CSE pass。这是**编译期 emission 的责任**,不是优化器的事。

## 8. 链式复合赋值

`a op= b op= c` 右结合解析为 `a op= (b op= c)`(见 `docs/grammar.txt`)。每个 compound 各自保证自己 locator 只求值一次;内层 `b op= c` 作为外层 rhs 求值(其值为赋值后的新 `b`)。链式不引入新的重复求值问题,沿用上述 lowering 即可。`lvalue_mode_` 由目标节点入口 `take_lvalue_mode()` 清空为 `Load`,故内层赋值完整结束(已清空)后外层才发其 `emit_lvalue`,嵌套不泄漏 flag。

## 9. 备选方案(未采纳)

locator 溢出到临时 local 槽:用现有操作码(`STORE_LOCAL`/`LOAD_LOCAL` 到编译器分配的临时槽)代替 `DUP`/`DUP2`。优点是无需新操作码;缺点是 ops 更多、需分配并管理临时槽生命周期。本设计选 `DUP`/`DUP2`:ops 少、无临时槽、与 Python 成熟方案一致。
