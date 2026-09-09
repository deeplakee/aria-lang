# LoopCtx 结构体说明

> 源码位置：`src/compile/FunctionCtx.hpp`
> 相关代码：`src/compile/CodeGen.cpp`（`visitWhileStmtNode` / `visitForStmtNode` / `visitForInStmtNode` / `visitBreakStmtNode` / `visitContinueStmtNode`）

## 1. 它解决什么问题

aria 的 `break` / `continue` 是**前向跳转**（break 跳到循环结束 `L_end`）或**后向跳转**（continue 跳回循环开头 / 递增区 `L_incr`），但循环的**结束地址**和**递增区地址**在编译到 `break`/`continue` 那一刻往往还不知道（它们在循环体之后才发射）。

clox 风格的解法是「**占位 + 回填**」：遇到 `break`/`continue` 先发一条 `JUMP`，把这条跳转指令的**占位偏移**记下来；等循环体编译完、真正的目标地址确定了，再回去把这些占位改成真实偏移（`patch_jump`）。

`LoopCtx` 就是记录「**当前这一个循环**」在编译期内需要的所有回填信息的载体。每个循环进入时往 `FunctionCtx::loop_stack_` 压一个 `LoopCtx`，循环结束时弹出并完成所有回填。

## 2. 字段逐项说明

```cpp
struct LoopCtx {
    u32         loop_scope_depth     = 0;            // 循环体所在 scope 深度（break/continue 弹局部至此）
    Opt<u32>    continue_back_target = std::nullopt; // 后向 continue 目标（while / for-in / for 无 incr）
    List<usize> continue_fwd_patches;                // 前向 continue 回填（for 有 incr -> L_incr）
    List<usize> break_fwd_patches;                   // 待回填的 JUMP 占位偏移
};
```

`LoopCtx` 经头文件内联工厂 `make_loop_ctx(loop_scope_depth)` 构造（`continue_back_target=nullopt`、两个 patch 列表空，全字段显式初始化收口于一处），调用方按循环类型在入栈前给 `continue_back_target` 赋值。字段语义如下：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `continue_back_target` | `Opt<u32>` | **后向** continue 的目标地址（字节码偏移）。有值时 `continue` 直接 `emit_jump_back` 回跳，无需回填。用于 continue 目标**在 continue 之前**的情况：`while` / `for-in`（回 `L_start`）、无 `increment` 的 `for`（回 `L_cond`）。 |
| `continue_fwd_patches` | `List<usize>` | **前向** continue 的待回填 `JUMP` 占位偏移列表。continue 目标在 continue **之后**时用：即 `for` 带 `increment` 的场合，continue 要跳到 `L_incr`（递增区），而 `L_incr` 此刻还没发射，所以先发占位 `JUMP`、记偏移，等循环体编译完到 `L_incr` 时统一回填。 |
| `loop_scope_depth` | `u32` | 循环**体**所在的块作用域深度。`break`/`continue` 跳出循环体前，要把循环体内声明的局部变量弹出（`POP_N`），弹到这个深度为止。注意它记录的是循环**进入前**的 scope 深度（即循环体作用域的外层），不是循环体内部新开的 scope。 |
| `break_fwd_patches` | `List<usize>` | 待回填的 `break` 的 `JUMP` 占位偏移列表。`break` 永远是前向跳（跳到 `L_end`），所以一律走占位 + 回填。 |

### `continue_back_target` vs `continue_fwd_patches` 为什么是二选一

这是 `LoopCtx` 最关键的设计点：**continue 的目标地址在遇到 continue 时是否已知**，决定了走哪条路径。

- **已知（后向）**：目标在 continue 之前已经发射。填 `continue_back_target`，`continue` 直接 `emit_jump_back(target_off)` 一条指令搞定，`continue_fwd_patches` 留空。
- **未知（前向）**：目标在 continue 之后才发射（`for` 的递增区 `L_incr`）。`continue_back_target` 留空（`Opt{}` = nullopt），`continue` 发一条占位 `JUMP` 并把偏移塞进 `continue_fwd_patches`，等循环体编译完再统一回填。

二者互斥：构造 `LoopCtx` 时根据循环类型只填其中一个，另一个保持空。`visitContinueStmtNode` 里用 `if (loop.continue_back_target)` 判断走哪条分支。

## 3. 在代码中的实际使用

`LoopCtx` 不被直接构造后长期持有，而是由 `CodeGen` 的循环 visit 函数**入栈 -> 编译循环体 -> 出栈并回填**的标准三段式使用。下面按三种循环 + break/continue 分别看。

### 3.1 统一使用模式

所有循环 visit 都遵循这个骨架：

```cpp
const u32 loop_scope = cur_fn_ctx()->scope_depth_;  // 记录循环体所在 scope 深度
// ... 发射循环条件/初始化，拿到 continue 的后向目标（若有）...
auto loop_ctx = make_loop_ctx(loop_scope);          // 工厂全字段初始化（back_target=nullopt、两列表空）
loop_ctx.continue_back_target = <continue_target>;  // 按循环类型赋值（for 有 incr 留空）
cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
emit_stmt(node->body.get());                        // 编译循环体（体里的 break/continue 会读栈顶 LoopCtx）
const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);  // 循环结束，取出并弹出
// ... 发射回边 / 递增 / 回填 break_fwd_patches（和 continue_fwd_patches）...
```

`break`/`continue` 在循环体内被访问时，总是取 `loop_stack_.top()`（**当前最内层**循环的 `LoopCtx`），往它的 `break_fwd_patches` / `continue_fwd_patches` 里追加占位偏移，或用 `continue_back_target` 直接回跳。这天然实现了「break/continue 绑定到最内层循环」。

### 3.2 `while` 循环（`visitWhileStmtNode`，CodeGen.cpp:496）

```cpp
const u32 l_start = cur_cu()->size();      // 循环起点 = continue 的后向目标
emit_expr(node->condition.get());
const auto jf = cur_cu()->emit_jump(OpCode::JUMP_FALSE, line); // -> L_end

auto loop_ctx = make_loop_ctx(cur_fn_ctx()->scope_depth_);
loop_ctx.continue_back_target = l_start;
cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
emit_stmt(node->body.get());
const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

emit_jump_back_or_fail(l_start, line, node->loc());  // 回边 -> L_start
patch_jump_or_fail(jf, node->loc());                 // 条件假 -> L_end
for (const auto bp: loop.break_fwd_patches)          // 回填所有 break -> L_end
    patch_jump_or_fail(bp, node->loc());
```

- `continue_back_target = l_start`（continue 回到条件判断处，后向，直接回跳）。
- `continue_fwd_patches` 空。
- `break_fwd_patches` 在出循环后统一回填到此处（即 `L_end`）。

字节码布局：
```
L_start: <cond> JUMP_FALSE -> L_end
         <body>（break -> JUMP 占位，记入 break_fwd_patches）
         JUMP_BACK -> L_start
L_end:   <break 回填到这里>
```

### 3.3 `for` 循环（`visitForStmtNode`，CodeGen.cpp:515）

这是最复杂的，因为 continue 的目标取决于**有没有 increment**：

```cpp
begin_scope();
// ... init ...
const u32 l_cond = cur_cu()->size();
// ... condition -> JUMP_FALSE -> L_end ...
const bool has_incr = node->increment != nullptr;
auto loop_ctx = make_loop_ctx(loop_scope);
if (!has_incr) {
    loop_ctx.continue_back_target = l_cond;  // 无 incr: continue 后向跳 L_cond
} // 有 incr: 留空，走前向 continue_fwd_patches -> L_incr
cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
emit_stmt(node->body.get());
const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);

// *** 关键：前向 continue 必须在「递增发射前」回填（此刻 size() 即 L_incr）***
for (const auto cp: loop.continue_fwd_patches)
    patch_jump_or_fail(cp, node->loc());   // -> L_incr
// ... emit increment; POP ...
emit_jump_back_or_fail(l_cond, line, node->loc());  // 回边 -> L_cond
// ... patch_jump(jf) -> L_end ...
for (const auto bp: loop.break_fwd_patches) // 回填 break -> L_end
    patch_jump_or_fail(bp, node->loc());
end_scope(line);
```

**两个要点**：

1. **`continue_back_target` 条件填**：有 `increment` 时留空（走前向 `continue_fwd_patches` 跳 `L_incr`），无 `increment` 时填 `l_cond`（走后向回跳）。
2. **前向 continue 回填时机**：`continue_fwd_patches` 必须在 `cur_cu()->size() == L_incr` 即**递增区发射之前**回填（CodeGen.cpp 注释专门强调）。若等递增和 `JUMP_BACK` 都发完再回填，`size()` 已经是 `L_end`，continue 会错跳到 `L_end` 提前退出循环。

字节码布局（有 incr）：
```
        <init>
L_cond: <cond> JUMP_FALSE -> L_end
        <body>（continue -> JUMP 占位入 continue_fwd_patches；break -> JUMP 占位入 break_fwd_patches）
L_incr: <incr> POP          <- continue_fwd_patches 回填到这里
        JUMP_BACK -> L_cond
L_end:  <- break_fwd_patches 回填到这里
```

### 3.4 `for-in` 循环（`visitForInStmtNode`，CodeGen.cpp:559）

与 `while` 同型：continue 后向跳回 `l_start`（每轮重新调 `has_next` 判断）。

```cpp
const u32 l_start = cur_cu()->size();      // has_next 判断处
// ... LOAD iter; has_next; CALL; JUMP_FALSE -> L_end ...
auto loop_ctx = make_loop_ctx(loop_scope);
loop_ctx.continue_back_target = l_start;
cur_fn_ctx()->loop_stack_.push(std::move(loop_ctx));
// ... next; bind_pattern; body ...
const auto loop = util::pop_top(cur_fn_ctx()->loop_stack_);
emit_jump_back_or_fail(l_start, line, node->loc());
patch_jump_or_fail(jf, node->loc());                  // -> L_end
for (const auto bp: loop.break_fwd_patches)
    patch_jump_or_fail(bp, node->loc());
```

`continue_back_target = l_start`，`continue_fwd_patches` 空，与 `while` 完全一致。

### 3.5 `break`（`visitBreakStmtNode`，CodeGen.cpp:610）

```cpp
if (cur_fn_ctx()->loop_stack_.empty())
    fail(ErrorCode::BreakOutsideLoop, node->loc(), "break 不在循环内");
auto& loop = cur_fn_ctx()->loop_stack_.top();      // 最内层循环
emit_pop_locals_to(loop.loop_scope_depth, line);         // 弹循环体内局部
loop.break_fwd_patches.push_back(                       // 发占位 JUMP，记偏移
    cur_cu()->emit_jump(OpCode::JUMP, line));       // -> L_end（待回填）
```

三步：① 查非空（否则 `BreakOutsideLoop`）；② 用 `loop_scope_depth` 弹局部；③ 发占位 `JUMP` 并把偏移追加到 `break_fwd_patches`。break 永远前向，所以从不需要 `continue_back_target`。

### 3.6 `continue`（`visitContinueStmtNode`，CodeGen.cpp:620）

```cpp
if (cur_fn_ctx()->loop_stack_.empty())
    fail(ErrorCode::ContinueOutsideLoop, node->loc(), "continue 不在循环内");
auto& loop = cur_fn_ctx()->loop_stack_.top();
emit_pop_locals_to(loop.loop_scope_depth, line);         // 弹循环体内局部
if (loop.continue_back_target) {                    // 后向：目标已知
    cur_cu()->emit_jump_back(*loop.continue_back_target, line);
} else {                                            // 前向：目标未知，占位待回填
    loop.continue_fwd_patches.push_back(
        cur_cu()->emit_jump(OpCode::JUMP, line));   // -> L_incr（待回填）
}
```

这里清晰地体现了 `continue_back_target` / `continue_fwd_patches` 的二选一分支：有后向目标就直接 `emit_jump_back`；没有就发占位 `JUMP` 入 `continue_fwd_patches`，交给 `for` 循环出体时回填。

## 4. 几个关键性质

### 4.1 随函数隔离（不跨函数绑定外层循环）

`loop_stack_` 是 `FunctionCtx` 的成员，**进新函数即得空 `loop_stack_`**（见 `FunctionCtx.hpp:14-15` 的注释）。所以嵌套函数里的 `break`/`continue` 不会绑到外层函数的循环--外层循环的 `LoopCtx` 在外层函数的 `loop_stack_` 里，子函数看不到。子函数顶层写 `break` 会因自己的 `loop_stack_` 为空而报 `BreakOutsideLoop`。测试 `ErrBreakInNestedFunDoesNotBindOuterLoop`（`tests/compile/test_codegen.cpp`）专门验证这一点。

### 4.2 `loop_scope_depth` 的作用：跳转前弹局部

`break`/`continue` 跳出循环体时，循环体内声明的局部变量在运行期已「离开作用域」，必须在跳转指令前用 `POP_N` 弹掉，否则栈会泄漏。`loop_scope_depth` 记录的是循环**体所在 scope 的外层深度**，`emit_pop_locals_to(loop_scope_depth)` 会弹掉所有比这更深的局部（即循环体内声明的局部），无论 `break`/`continue` 出现在循环体的哪一层嵌套块里。

注意 `emit_pop_locals_to` 只发射弹区清理指令（运行期弹栈），**不破坏编译期 `locals_` 登记**--M4 起 break/continue 与退出作用域共用同一发射口 `emit_pop_locals_to`（整区一条 `POP_N`，弹区含被捕获局部才追加一条批量 `CLOSE_UPVALUE`），它**只发射、不移除登记**；真正从 `locals_` pop_back 移除的只有退出作用域路径随后调的 `FunctionCtx::end_scope()`（`--scope_depth_` 后弹出原 scope 的局部；`CodeGen::end_scope` = 先 `emit_pop_locals_to` 再此收尾）。因为 `break`/`continue` 后的语句（死代码或其他分支）仍在作用域内，可引用这些局部。若 break/continue 路径误走了登记移除，循环体局部会被 `locals_` pop_back 掉，后续语句引用该名会误落全局（运行期 `UndefinedVariable`）。

### 4.3 入栈/出栈的 RAII 式对称

每个循环 visit 严格遵循「push -> 编译体 -> `util::pop_top` 取出并弹出」的对称结构。`pop_top` 经 `std::move` 取出栈顶再 `pop`，这样回填阶段用的是局部副本 `loop`，即使回填过程中 `cur_cu()` 状态变化也不影响 `loop_stack_`。出错路径下（`fail()` 抛 `AriaCompileException` unwind），pop 会被跳过，但整个 `ModuleCtx` 析构时会沿 enclosing_ 链清理 `FunctionCtx`，`loop_stack_` 作为其成员随之销毁，不泄漏。

### 4.4 为什么 `LoopCtx` 是简单聚合而不是带方法的类

`LoopCtx` 的全部行为（入栈、读栈顶、追加占位、回填）都由 `CodeGen` 编排，`LoopCtx` 本身只是数据载体。这与 `FunctionCtx` 的设计分工一致（见 `FunctionCtx.hpp:4-7` 注释）：`FunctionCtx` 负责「登记」（局部/作用域/循环栈管理），`CodeGen` 负责「发射」（`emit_op`/跳转回填/错误）。`LoopCtx` 作为 `FunctionCtx` 的成员，自然也只持数据、不持逻辑。回填逻辑（`patch_jump`）属于 `CodeUnit` 的编码能力，由 `CodeGen` 调用，不放进 `LoopCtx`。

## 5. 速查：三种循环的 `LoopCtx` 配置

| 循环类型 | `continue_back_target` | `continue_fwd_patches` | continue 实际跳到 |
| --- | --- | --- | --- |
| `while` | `l_start` | 空 | 条件判断处（后向） |
| `for`（有 incr） | 空 | 用到 | `L_incr` 递增区（前向，回填） |
| `for`（无 incr） | `l_cond` | 空 | 条件判断处（后向） |
| `for-in` | `l_start` | 空 | `has_next` 判断处（后向） |

`break_fwd_patches` 三者都用，出循环后统一回填到 `L_end`。

## 6. 从编译顺序理解字段设计

第 3 节按「用法」讲，本节按「**编译器真正发射字节码的时间顺序**」讲，回答一个更根本的问题：`LoopCtx` 四个字段为什么是这个形态（单值 `Opt` vs 列表 `List`）。答案由一个事实决定 -- **在编译到 `break`/`continue` 那一刻，它的跳转目标地址是「已经发射过（已知）」还是「还没发射（未知）」**。

### 6.1 核心原则：dst 先定还是后定

跳转由两个端点组成：**src**（发射跳转指令的位置）和 **dst**（跳转目标地址）。谁先确定，就把谁「随身携带」到另一个端点处完成对接。底层三个原语（`CodeUnit` 提供）：

- `emit_jump(op)` -- 发 `op` + 2 字节**占位**，返回占位偏移 src_off。dst 未知时用。
- `patch_jump(src_off)` -- 用「**当前**已发射到的位置」回填占位。前向跳，dst 在后面、此刻才确定时用。
- `emit_jump_back(target_off)` -- 一次性发后向跳，target_off 必须**已经发射过**（已知）。

| dst 在 src 编译时是否已知 | 用法 | LoopCtx 对应字段 | 存的是 |
| --- | --- | --- | --- |
| **已知**（dst 在前面已发射） | `emit_jump_back(dst)` 一条闭环，src 用完即弃 | `continue_back_target`（单值 `Opt<u32>`） | **dst**（唯一且先知） |
| **未知**（dst 在后面才发射） | `emit_jump` 占位 + 记 src 偏移 + 事后 `patch_jump` | `continue_fwd_patches` / `break_fwd_patches`（列表） | **src 们**（多个，攒到 dst 处回填） |

字段存什么 = 「先确定的端点是单个 dst 还是多个 src」的镜像：
- **后向（dst 先定）**：dst 唯一且先知，存这一个 dst 值就够；每个 src 编译时 dst 在手边，直接回跳，src 不用存 -> 单值 `Opt`。
- **前向（dst 后定）**：dst 此刻未知，每个 src 只能先发占位、把自己的偏移记下来，攒到 dst 处统一回填 -> src 列表 `List`。

字段名也把这个语义写进去了：`continue_back_target` 末尾 `target` = dst；`*_patches` 末尾 `patches` = 待回填的 src 占位偏移。

### 6.2 `for (init; cond; incr) body` -- 带 increment（最复杂）

唯一让 continue 走「前向 + 回填」的循环，因为 **continue 必须先执行 `incr` 再回到条件**，而 `incr` 的字节码排在循环体**之后**。

编译时间线（↓ 表示发射顺序）：

```
begin_scope()                         // 开 for 自己的作用域，loop_scope = 此刻深度
emit <init>                           // init 局部声明在 loop_scope
L_cond = size()  ◀───────────────┐   记下条件起点
emit <cond>                       │
JUMP_FALSE ──► ? (jf 占位)        │   条件假跳 L_end（前向，后面才知）
                                  │
push LoopCtx{                     │   ┌ continue_back_target = none（有 incr，走前向）
  continue_back_target = none,    │   │ continue_fwd_patches  = {}
  continue_fwd_patches = {},      │   │ loop_scope_depth      = loop_scope
  loop_scope_depth = loop_scope,  │   │ break_fwd_patches         = {}
  break_fwd_patches = {}              │   └
}                                 │
emit <body>  ◄── break/continue 在这里编译：
  │   break:    emit_pop_locals_to(loop_scope); emit_jump(JUMP) -> break_fwd_patches
  │   continue: emit_pop_locals_to(loop_scope); emit_jump(JUMP) -> continue_fwd_patches
  │                                  │     （有 incr，故走前向占位，记进 fwd_patches）
pop LoopCtx -> loop                │
                                  │
L_incr = size()  ◀────────────────┘   记下递增区起点 = 前向 continue 的 dst
patch 全部 continue_fwd_patches -> L_incr   ★必须在发射 incr 之前回填★
emit <incr>; POP                      发递增
JUMP_BACK ──► L_cond                   回边，重新判断条件
L_end = size()
patch jf -> L_end
patch 全部 break_fwd_patches -> L_end
end_scope()                           // 弹 init 局部（depth = loop_scope）
```

成品字节码布局（地址从上到下递增）：

```
        <init>
L_cond: <cond>  JUMP_FALSE -> L_end
        <body>          （break -> JUMP 占位； continue -> JUMP 占位）
L_incr: <incr>  POP     <- continue_fwd_patches 回填到这里
        JUMP_BACK -> L_cond
L_end:  <- jf / break_fwd_patches 回填到这里
```

三个关键点：

1. **continue 跳 `L_incr` 而非 `L_cond`**：C 风格 for 的 continue 语义是「跳过本轮剩余体，但**仍要执行递增**再判断」。目标必须是递增区起点 `L_incr`；而 `L_incr` 在循环体之后才发射，continue 编译时还不知道 -> 占位 + 回填 -> `continue_fwd_patches`。这是 `for` 带 increment 独有前向 continue 的全部根因。
2. **`continue_fwd_patches` 必须在「发射 incr 之前」回填**：`patch_jump` 用「当前 `size()`」当 dst。回填那一刻 `size()` 必须恰好等于 `L_incr`，所以顺序是「`L_incr = size()` -> 立刻回填 -> 再发 incr」。若等 incr 与 `JUMP_BACK` 发完再回填，`size()` 已是 `L_end`，continue 全错跳到 `L_end` 直接退出循环（`CodeGen.cpp:539-540` 注释专门强调）。
3. **`continue_back_target = none`**：有 incr 时 continue 走前向，后向目标留空，`visitContinueStmtNode` 里 `if (loop.continue_back_target)` 为假 -> 走 else 分支追加 `continue_fwd_patches`。

### 6.3 `while (cond) body` -- continue 后向

while 没有「递增」步，continue = 「重新判断条件」，条件在循环体**之前**发射 -> dst 已知 -> 后向直接回跳。

```
L_start = size()  ◀─────────────┐   continue 的 dst（已知）
emit <cond>                     │
JUMP_FALSE ──► ? (jf 占位)      │
push LoopCtx{                   │
  continue_back_target = L_start, │   <- 有值，存 dst
  continue_fwd_patches = {},   │
  loop_scope_depth = loop_scope, │
  break_fwd_patches = {}           │
}                               │
emit <body>  ◄── break: emit_jump -> break_fwd_patches
  │             continue: emit_jump_back(L_start)   <- 一条指令，无需回填
pop LoopCtx -> loop              │
JUMP_BACK ──► L_start           │   回边
L_end = size()
patch jf -> L_end
patch break_fwd_patches -> L_end
```

`continue_back_target = L_start` 有值 -> continue 走 `emit_jump_back`，不碰 `continue_fwd_patches`（空）。`break_fwd_patches` 仍用，因为 break 的 dst `L_end` 在体后，永远前向。

### 6.4 `for-in (x in iter) body` -- 与 while 同型

for-in 也没有独立递增步，continue = 「重新调 `has_next` 判断」，判断在体前 -> 后向已知。

```
... 声明 <iter>（值填槽：iterable.iter() 出值即 <iter>，无 LOAD_NIL 预占），取 iter 对象 ...
L_start = size()  ◀─────────────┐   continue 的 dst（已知）
LOAD iter; has_next; CALL;       │
JUMP_FALSE ──► ? (jf 占位)      │
push LoopCtx{                   │
  continue_back_target = L_start, │
  continue_fwd_patches = {},   │
  loop_scope_depth = loop_scope, │
  break_fwd_patches = {}           │
}                               │
emit next; bind_pattern; body   │   break -> break_fwd_patches; continue -> emit_jump_back(L_start)
pop LoopCtx -> loop              │
JUMP_BACK ──► L_start           │
L_end = size()
patch jf; patch break_fwd_patches -> L_end
end_scope()
```

与 while 完全同型：`continue_back_target` 有值、`continue_fwd_patches` 空。

### 6.5 `for (init; cond) body` -- 无 increment

`visitForStmtNode` 里 `has_incr = (node->increment != nullptr)`。无 incr 时 continue 跳回 `L_cond` 重新判断（没有递增要跑），`L_cond` 在体前 -> 后向已知。

构造时经 `make_loop_ctx(loop_scope)` 建默认上下文后，无 incr 再给 `continue_back_target` 赋 `l_cond`；有 incr 留空。其余与带 incr 的 for 一致，只是没有 `L_incr` 那段、没有 `continue_fwd_patches` 要回填。

### 6.6 字段形态的统一解释

把四种情况摆一起，设计逻辑收口：

| 循环 | continue 的 dst | dst 在 src 编译时已知? | continue 走哪条 | `continue_back_target` | `continue_fwd_patches` |
| --- | --- | --- | --- | --- | --- |
| `while` | `L_start`（条件） | 已知（在前） | `emit_jump_back` | `= L_start` | 空 |
| `for-in` | `L_start`（has_next） | 已知（在前） | `emit_jump_back` | `= L_start` | 空 |
| `for` 无 incr | `L_cond` | 已知（在前） | `emit_jump_back` | `= L_cond` | 空 |
| `for` 有 incr | `L_incr`（递增区） | **未知**（在后） | 占位 + 回填 | 空 | **用到** |

- **`continue_back_target` 是 `Opt<u32>`（单值）**：后向场景 dst 唯一且先知，continue 直接 `emit_jump_back(*target)`，src 用完即弃，一个 dst 值够用，不需要列表。
- **`continue_fwd_patches` 是 `List<usize>`（列表）**：前向场景 dst 未知，每个 continue 各发一个占位、各记一个 src 偏移，攒到循环体编译完再批量回填到 `L_incr`，所以是列表。两者互斥，构造时按循环类型只填一个。
- **`break_fwd_patches` 与 `continue_fwd_patches` 是同一类东西**：都是「前向跳转、src 先发、dst 后定」，所以都是 src 列表。差别只在 dst 是什么（`L_end` vs `L_incr`）、以及 dst 在哪一刻变得已知（break 的 `L_end` 在循环全部编译完时；continue 的 `L_incr` 在体编译完、递增区发射前那一刻，故 continue 的回填要先于 break 的回填）。
- **没有 `break_back_target`** 是同一条规则的推论：break 的 dst 恒为 `L_end`，**永远在循环体之后**，不可能先确定，所以 break 没有后向分支、没有 dst 字段，只有 src 列表。
- **`loop_scope_depth` 与跳转方向无关**：它给 `emit_pop_locals_to` 用 -- break/continue 跳出循环体前要把循环体内声明的局部用 `POP_N` 弹掉（否则栈泄漏）。它记录循环体外层的 scope 深度，弹掉所有比它更深的局部（即循环体里声明的）。四种循环都填同一个值（进入循环体前的 `scope_depth_`），与 dst 是否先知无关。

一句话：`continue_back_target` 存 dst（单值，因 dst 唯一且先知），`continue_fwd_patches` / `break_fwd_patches` 存 src 们（列表，因 dst 后知、要把多个 src 攒到 dst 处回填）。`for` 带 increment 是唯一让 continue 的 dst 落在体后的循环，这就是它独有 `continue_fwd_patches` 路径的全部原因。