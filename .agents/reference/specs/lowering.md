# CodeGen lowering 规格

`CodeGen` 一次 AST 遍历里两处有独立机制的 lowering：**赋值类表达式的 locator-once** 与 **循环的 break/continue 回填**。文法层的不变式见 `docs/grammar.txt`；本文是编译期实现方案。

## 一、复合赋值与前置 `++`/`--`

### 约束

`E1 op= E2` 等价于 `E1 = E1 op E2`，但**左值只求值一次**（C 标准定义）。`arr[f()] += 1` 的 `f()` 只调一次、`getObj().field += 1` 的 `getObj()` 只调一次。读和写本来就是两次操作，要避免的只是「决定写到哪」的子表达式跑多次：**把 locator 与 load/store 分开，locator 只算一次**。

`++E` / `--E` 同属此列（C 标准明文等价于 `E += 1` / `E -= 1`），只有两点差别：运算对象是常量 1，且作为表达式**返回新值**。

### 左值形态与 locator

合法左值 = `identifier / obj.field / obj[index]`（合法性在语义阶段校验，`validate_lvalue_target` 拒其余形态，报 `InvalidAssignmentTarget`）。

| 左值形式 | locator 的运行时部分 | 保留手法 |
| :--- | :--- | :--- |
| `identifier`（局部 / upvalue / global） | 无（槽位号 / upvalue 索引 / 全局名都是编译期常量） | 无 |
| `obj.field` | `obj`（字段名是编译期常量） | `DUP` |
| `obj[idx]` | `obj` + `idx`（都可能带副作用） | `DUP2` |

`DUP`（`[a] -> [a, a]`）与 `DUP2`（`[a, b] -> [a, b, a, b]`）是无操作数的通用栈操作码；复制数量编进码名、不带操作数（区别于带操作数的 `POP_N`）。

### `LvalueMode` 与三腿

`CodeGen` 成员 `lvalue_mode_`（默认 `Load`）告诉目标节点当前是读还是写：`emit_lvalue(node, mode)` 先 `validate_lvalue_target`、再设 flag、再 `node->accept(*this)`；目标节点入口用 `take_lvalue_mode()` 一次性取走并复位为 `Load`，故子表达式经 `emit_expr` 时 flag 已清空、不会泄漏；`emit_expr` 入口 `ASSERT(lvalue_mode_ == Load)` 开发期抓漏 take。

三种生产形态只差首腿与中间腿：

| 形态 | 序列 |
| :--- | :--- |
| 普通 `=` | `emit_lvalue(lhs, Prepare)` -> `<e>` -> `emit_lvalue(lhs, Store)` |
| 复合 `op=` | `emit_lvalue(lhs, Locate)` -> `<e>` -> `<op>` -> `emit_lvalue(lhs, Store)` |
| 前置 `++`/`--` | `emit_lvalue(lhs, Locate)` -> `LOAD_IMM 1` -> `<op>` -> `emit_lvalue(lhs, Store)` |

- **Store 腿一律 peek-store**：`STORE_LOCAL`/`STORE_GLOBAL`/`STORE_UPVALUE`/`STORE_FIELD`/`STORE_INDEX` 写回后**留新值在栈上**，即赋值表达式的值；语句上下文（值被丢弃）由编译器补一条 `POP`。故本文各序列末尾栈形都是「新值」，不是空。
- **Prepare 腿**只发 locator 的运行时部分（`obj` / `obj idx`）；Identifier 与帧内 `this` 的 locator 是编译期常量，Prepare 臂是 no-op。`STORE_FIELD` 的栈形 `[obj, v] -> [v]` 要求接收者先于值入栈，发射权归目标节点。
- **Locate 腿**按 locator 性质两路：编译期常量（Identifier、帧内 `this`）与 Load 同形--重 resolve 免费且无副作用；运行时 locator（Field/Index）单次求值后用 `DUP`/`DUP2` 留栈，Load 与 Store 复用栈上副本。都不靠编译期缓存 locator 描述。
- `super.成员` 不是 `FieldAccessNode`（独立 `SuperExprNode`，Load-only），写形态被 validate 拒绝。
- 局部/全局的读写腿实际经 `CodeUnit::emit_load_local`/`emit_store_local` 发射，槽 1..8 会落成零操作数的 `LOAD/STORE_LOCAL_k` 短变体（下表按通用形态写）。

### 目标节点组合速查

| 目标 · 形态 | Prepare | Load | Store | Locate |
| :--- | :--- | :--- | :--- | :--- |
| Identifier（任意种类） | no-op | `LOAD_*` | `STORE_*`（peek-store） | 与 Load 同形 |
| FieldAccess · 帧内 `this` | no-op | `LOAD_THIS_FIELD` | `STORE_THIS_FIELD`（字段不存在则动态创建） | 与 Load 同形（this 取帧槽 0，发 DUP 副本反而无人消费） |
| FieldAccess · 一般对象 / 捕获 this | `<object>` | `<object>` + `LOAD_FIELD` | 只发 `STORE_FIELD` | `<object>` + `DUP` + `LOAD_FIELD` |
| IndexAccess | `<obj>` + `<idx>` | `<obj>` + `<idx>` + `LOAD_INDEX` | 只发 `STORE_INDEX` | `<obj>` + `<idx>` + `DUP2` + `LOAD_INDEX` |

Identifier 的 upvalue 写腿不查 init、不 mark（写穿外层槽或已关 cell）。

### 逐行示例

`obj.f += e`（Locate 的 DUP 副本由 Store 腿消费）：

```
<object>            ; [obj]               Locate 腿
DUP                 ; [obj, obj]          Locate 腿：副本垫底
LOAD_FIELD f        ; [obj, obj.f]        Locate 腿：读旧值
<e>                 ; [obj, obj.f, e]     值腿（emit_expr，恒 Load 语境）
ADD                 ; [obj, new]          运算（无模式）
STORE_FIELD f       ; [new]               Store 腿：弹 (副本, 新值)，留新值
```

`this.x += e`（帧内 this，全程无 DUP）：

```
LOAD_THIS_FIELD x   ; [old]               Locate 腿（折叠成 THIS_FIELD Load 形）
<e>                 ; [old, e]
ADD                 ; [new]
STORE_THIS_FIELD x  ; [new]               Store 腿：peek-store，this 取帧槽 0
```

`obj[idx] += e`（`idx` 的副作用只发生一次）：

```
<obj>               ; [obj]
<idx>               ; [obj, idx]          副作用在此，仅一次
DUP2                ; [obj, idx, obj, idx]
LOAD_INDEX          ; [obj, idx, old]
<e>                 ; [obj, idx, old, e]
ADD                 ; [obj, idx, new]
STORE_INDEX         ; [new]
```

普通 `obj.f = e`（Prepare 只发接收者，Store 只发一条）：

```
<object>            ; [obj]               Prepare 腿
<e>                 ; [obj, e]
STORE_FIELD f       ; [e]
```

### 链式

`a op= b op= c` 右结合解析为 `a op= (b op= c)`；内层作为外层 rhs 求值，其值为赋值后的新 `b`。每个 compound 各自保证自己的 locator 只求值一次，链式不引入新的重复求值。flag 由目标节点入口 take 清空，内层完整结束后外层才发自己的 `emit_lvalue`，嵌套不泄漏。

### 不要这么做

- **别在 AST 层把 `E1 op= E2` 重写成 `E1 = E1 op E2`** 再按普通赋值编译--等于把 locator 求值两次的责任硬塞回去。
- **别指望 CSE 消重**：CSE 只能消纯表达式，`f()` 有副作用；脚本 VM 也没有 CSE pass。这是 emission 的责任。
- **别用临时局部槽代替 `DUP`/`DUP2`**：可行但 ops 更多，还要管理临时槽生命周期。

## 二、break / continue 与 `LoopCtx`

`break` 是前向跳（到 `L_end`），`continue` 是后向跳（回循环头 / 递增区）；两者的目标地址在编译到该语句时往往还没发射。解法是「占位 + 回填」：先发 `JUMP` 并记下占位偏移，等目标地址确定再 `patch_jump` 回来改。循环头条件假跳的 `L_end` 同样后知，占位与 break 记在同一张表。

`LoopCtx`（`FunctionCtx::loop_stack_` 的元素）就是「当前这一个循环」的全部回填信息：

```cpp
struct LoopCtx {
    u32            loop_scope_depth = 0;   // 循环体所在 scope 深度（break/continue 弹局部至此）
    u32            back_target      = 0;   // 回边目标（条件求值点），后向 continue 亦跳此
    Opt<List<u32>> continue_fwd_patches;   // 前向 continue 占位；nullopt = 本循环无前向通道
    List<u32>      exit_fwd_patches;       // 条件假跳 + 各 break 的前向占位
};
```

### `back_target` 与 `continue_fwd_patches` 二选一

判据是「continue 的目标在遇到它时是否已知」：

- **已知（后向）**：目标已发射，`continue` 直接 `emit_jump_back(back_target)` 一条指令，`continue_fwd_patches` 留 `nullopt`。`while` / `for-in`（回 `L_start`）与无 `incr` 的 `for`（回 `L_cond`）属此类。
- **未知（前向）**：目标在 continue 之后才发射（`for` 的递增区 `L_incr`），入栈前 `emplace()` 打开通道，`continue` 发占位 `JUMP` 并把偏移塞进去，体编译完再统一回填。

分派就靠通道存在性：`visitContinueStmtNode` 里 `if (loop_ctx.continue_fwd_patches)`。`back_target` 恒设（回边要用），与后向 continue 目标同址，一个字段两用。

### 三种循环的配置

| 循环 | `back_target` | `continue_fwd_patches` | continue 实际跳到 |
| :--- | :--- | :--- | :--- |
| `while` | `L_start` | nullopt | 循环头（后向） |
| `for`（有 incr） | `L_cond` | emplace 打开 | `L_incr`（前向，回填） |
| `for`（无 incr） | `L_cond` | nullopt | 循环头（后向） |
| `for-in` | `L_start` | nullopt | `has_next` 判断处（后向） |

`exit_fwd_patches` 四者都用（条件假跳 + break），收尾统一回填到 `L_end`。

### 使用三段式

「入栈 -> 编译体 -> 出栈并回填」：以指定初始化构造 `LoopCtx{.loop_scope_depth, .back_target}`（仅 for 带 incr 再 `emplace()`）-> 条件假跳占位记入 `exit_fwd_patches` -> push 进 `loop_stack_` -> 编译体（体内 break/continue 取栈顶，天然绑最内层循环）-> `pop_top` 取回 ->（仅 for 有 incr）回填 `continue_fwd_patches` -> 回边 + `exit_fwd_patches` 统一回填。

- **条件假跳**：`while`/`for` 的条件经 `emit_cond_jump` 发射，`if`/`while` 的真值判定会融合成 `JUMP_NE`（与 `JUMP_FALSE` 同族，偏移语义一致），本文各图按 `JUMP_FALSE` 写。
- **`break`**：栈空报 `BreakOutsideLoop` -> `emit_pop_locals_to(loop_scope_depth)` 弹体内局部 -> 发占位 `JUMP` 入 `exit_fwd_patches`（dst 恒为体后的 `L_end`，永远后知）。
- **`continue`**：栈空报 `ContinueOutsideLoop` -> 弹局部 -> 有通道则发占位入 `continue_fwd_patches`，否则 `emit_jump_back(back_target)`。
- **`emit_pop_locals_to` 只发射、不移除登记**：它和退出作用域共用同一发射口（整区一条 `POP_N`，含被捕获局部才追加一条批量 `CLOSE_UPVALUE`）；真正从 `locals_` 摘除只由 `end_scope()` 做。因为 break/continue 之后的语句（死代码或其他分支）仍在作用域内，还要能引用这些局部。
- **随函数隔离**：`loop_stack_` 是 `FunctionCtx` 成员，进新函数即空，子函数里的 `break` 不会绑到外层循环（报 `BreakOutsideLoop`）。
- **出栈对称**：`pop_top` 经 move 取栈顶再 pop；回填阶段操作的是独立副本。出错路径（`AriaCompileException` unwind）跳过 pop，但 `ModuleCtx` 析构会沿链清理 `FunctionCtx`，不泄漏。
- `LoopCtx` 是纯聚合：入栈、读顶、追加占位、回填全由 `CodeGen` 编排，`patch_jump` 属 `CodeUnit` 的能力。

### `for (init; cond; incr) body`：唯一的前向 continue

continue 必须先执行 `incr` 再回条件判断，而 `incr` 的字节码排在循环体**之后**--这是唯一让 continue 的 dst 落在体后的循环。编译时间线（↓ = 发射顺序）：

```
begin_scope()                          // 开 for 自己的作用域，loop_scope = 此刻深度
emit <init>                            // init 局部声明在 loop_scope
L_cond = size()                        // 条件起点
emit <cond>; emit_cond_jump -> ?       // 假跳 L_end，占位记入 exit_fwd_patches
push LoopCtx{back_target=L_cond, continue_fwd_patches={}, exit_fwd_patches={假跳占位}}
emit <body>                            // break: pop_locals + JUMP -> exit_fwd_patches
                                       // continue: pop_locals + JUMP -> continue_fwd_patches
pop LoopCtx
L_incr = size()                        // 递增区起点 = 前向 continue 的 dst
patch 全部 continue_fwd_patches -> L_incr   ★必须在发射 incr 之前回填★
emit <incr>; POP
emit_loop_backedge_and_exits:          // 回边 -> L_cond
L_end = size()                         // 回边发射之后才定 L_end
patch 全部 exit_fwd_patches -> L_end   // 条件假跳 + break 们
end_scope()                            // 弹 init 局部（depth = loop_scope）
```

回填时机是承重约束：`L_incr = size()` 之后必须**立刻**回填 continue 占位，若等 incr 与回边发完再回填，`size()` 已经是 `L_end`，continue 会错跳出去提前退循环。

## 三、字段形态的设计依据

跳转有两个端点：src（发射跳转处）与 dst（目标）。谁先确定，就把谁随身携带到另一端对接。三个原语（`CodeUnit`）：`emit_jump(op)` 发 op + 2 字节占位并返回 src_off（dst 未知时用）；`patch_jump(src_off)` 用**当前**已发射位置回填（前向、dst 此刻才确定时用，故没有 target 参数）；`emit_jump_back(target_off)` 一次性发后向跳（target 必须已发射）。

| dst 在 src 编译时是否已知 | 用法 | 对应字段 | 存的是 |
| :--- | :--- | :--- | :--- |
| 已知（已发射） | `emit_jump_back(dst)`，src 用完即弃 | `back_target`（单值） | **dst** |
| 未知（后发射） | `emit_jump` 占位 + 记 src 偏移 + 事后 `patch_jump` | `continue_fwd_patches` / `exit_fwd_patches`（列表） | **src 们** |

字段名即语义：`back_target` 的 `target` = dst，`*_patches` 的 `patches` = 待回填的 src 占位。两条推论：**没有 break 的后向字段**（dst 恒为体后的 `L_end`，不可能先知）；**`loop_scope_depth` 与跳转方向无关**（只给 `emit_pop_locals_to` 用，四种循环都填进入循环体前的 `scope_depth_`）。
