# LoopCtx 结构体说明

> 源码位置：`src/compile/FunctionCtx.hpp`
> 相关代码：`src/compile/CodeGen.cpp`（`visitWhileStmtNode` / `visitForStmtNode` / `visitForInStmtNode` / `visitBreakStmtNode` / `visitContinueStmtNode`）

## 1. 它解决什么问题

aria 的 `break` / `continue` 是**前向跳转**（break 跳到循环结束 `L_end`）或**后向跳转**（continue 跳回循环开头 / 递增区 `L_incr`），但循环的**结束地址**和**递增区地址**在编译到 `break`/`continue` 那一刻往往还不知道（它们在循环体之后才发射）。

clox 风格的解法是「**占位 + 回填**」：遇到 `break`/`continue` 先发一条 `JUMP`，把这条跳转指令的**占位偏移**记下来；等循环体编译完、真正的目标地址确定了，再回去把这些占位改成真实偏移（`patch_jump`）。循环头的条件假跳 `JUMP_FALSE` 同属此法（目标 `L_end` 同样后知），占位与 break 一并记入 `exit_fwd_patches`。

`LoopCtx` 就是记录「**当前这一个循环**」在编译期内需要的所有回填信息的载体。每个循环进入时往 `FunctionCtx::loop_stack_` 压一个 `LoopCtx`，循环结束时弹出并完成所有回填。

## 2. 字段逐项说明

```cpp
struct LoopCtx {
    u32              loop_scope_depth = 0;            // 循环体所在 scope 深度（break/continue 弹局部至此）
    u32              back_target      = 0;            // 循环头（条件求值点）：回边恒跳此，后向 continue 亦跳此
    Opt<List<u32>>   continue_fwd_patches;            // 前向 continue 回填（for 有 incr -> L_incr）；
                                                      // nullopt = 本循环无前向通道（continue 后向跳 back_target）
    List<u32>        exit_fwd_patches;                // 前向退出占位（循环头条件假跳 JUMP_FALSE + 各 break 的 JUMP）
};
```

`LoopCtx` 由调用点以指定初始化 `LoopCtx{.loop_scope_depth=..., .back_target=...}` 构造（其余成员默认初始化：`continue_fwd_patches=nullopt`、`exit_fwd_patches` 空）。入栈前仅 for 带 incr `emplace()` 打开 `continue_fwd_patches`，并把循环头条件假跳占位登记进 `exit_fwd_patches`（break 占位在体编译期间由 `visitBreakStmtNode` 追加）。

### `back_target` 与 `continue_fwd_patches` 的二选一

这是 `LoopCtx` 最关键的设计点：**continue 的目标地址在遇到 continue 时是否已知**，决定了走哪条路径。

- **已知（后向）**：目标在 continue 之前已经发射。`continue` 直接 `emit_jump_back(loop_ctx.back_target)` 一条指令搞定，`continue_fwd_patches` 留空。用于 `while` / `for-in`（回 `L_start`）、无 `increment` 的 `for`（回 `L_cond`）--三者的 `back_target` 即 continue 目标。
- **未知（前向）**：目标在 continue 之后才发射（`for` 的递增区 `L_incr`）。`continue_fwd_patches` 在入栈前 `emplace()` 打开，`continue` 发一条占位 `JUMP` 并把偏移塞进 `continue_fwd_patches`，等循环体编译完再统一回填。

分派由 `continue_fwd_patches` 通道的存在性承担：`visitContinueStmtNode` 里用 `if (loop_ctx.continue_fwd_patches)` 判断--有通道就发占位入通道，没有就直接回跳。`back_target` 恒设（回边用它），与后向 continue 的目标语义同址（都是"重新求值条件处"），一个字段两用。

## 3. 在代码中的实际使用

`LoopCtx` 由 CodeGen 的循环 visit 按「入栈 -> 编译循环体 -> 出栈并回填」三段式使用：以指定初始化构造 `LoopCtx{.loop_scope_depth, .back_target}`（仅 for 带 incr 再 `emplace()` 打开前向通道）-> 循环头条件假跳占位登记进 `exit_fwd_patches` -> push 进 `loop_stack_` -> 编译体（体内 break/continue 恒取栈顶 LoopCtx，天然绑定最内层循环）-> `pop_top` 出栈取回 ->（仅 for 有 incr）回填 `continue_fwd_patches` -> `emit_loop_backedge_and_exits` 回边 + exit 统一回填。

- **`while` / `for-in`**：`back_target` = 条件 / has_next 判断处，continue 后向直跳回循环头。布局：`L_start: <cond> JUMP_FALSE -> L_end; <body>（break 占位）; JUMP_BACK -> L_start`。
- **`for`（有 incr）**：`back_target` = `L_cond`；前向 continue 发占位入 `continue_fwd_patches`，**必须在递增区发射之前回填**（此刻 `size()` 即 `L_incr`；若等递增与回边发完再回填，`size()` 已是 `L_end`，continue 会错跳 L_end 提前退出循环）。布局：`<init>; L_cond: <cond> JUMP_FALSE -> L_end; <body>; L_incr: <incr> POP <- continue 回填处; JUMP_BACK -> L_cond`。无 incr 与 while 同型（continue 后向回 `L_cond`）。
- **`break`**：查栈非空（否则 `BreakOutsideLoop`）-> `emit_pop_locals_to(loop_scope_depth)` 弹体内局部 -> 发占位 `JUMP` 入 `exit_fwd_patches`（break 永远前向，dst 恒为体后 `L_end`）。
- **`continue`**：查栈非空（否则 `ContinueOutsideLoop`）-> 弹局部 -> 前向通道已打开则发占位入 `continue_fwd_patches` 待回填，否则 `emit_jump_back(back_target)`（§2 二选一的落点）。

## 4. 几个关键性质

### 4.1 随函数隔离（不跨函数绑定外层循环）

`loop_stack_` 是 `FunctionCtx` 的成员，**进新函数即得空 `loop_stack_`**。嵌套函数里的 `break`/`continue` 不会绑到外层函数的循环--外层循环的 `LoopCtx` 在外层函数的 `loop_stack_` 里，子函数看不到；子函数顶层写 `break` 会因自己的 `loop_stack_` 为空而报 `BreakOutsideLoop`。测试 `ErrBreakInNestedFunDoesNotBindOuterLoop`（`tests/compile/test_codegen.cpp`）专门验证这一点。

### 4.2 `loop_scope_depth` 的作用：跳转前弹局部

`break`/`continue` 跳出循环体时，循环体内声明的局部变量在运行期已「离开作用域」，必须在跳转指令前用 `POP_N` 弹掉，否则栈会泄漏。`loop_scope_depth` 记录的是循环**体所在 scope 的外层深度**，`emit_pop_locals_to(loop_scope_depth)` 会弹掉所有比这更深的局部（即循环体内声明的局部），无论 `break`/`continue` 出现在循环体的哪一层嵌套块里。

注意 `emit_pop_locals_to` **只发射、不移除登记**（M4 起与退出作用域共用同一发射口：整区一条 `POP_N`，弹区含被捕获局部才追加一条批量 `CLOSE_UPVALUE`）；真正从 `locals_` 移除的只有退出作用域路径随后调的 `FunctionCtx::end_scope()`。因为 `break`/`continue` 后的语句（死代码或其他分支）仍在作用域内，可引用这些局部。若 break/continue 路径误走了登记移除，循环体局部会被 `locals_` pop_back 掉，后续语句引用该名会误落全局（运行期 `UndefinedVariable`）。

### 4.3 入栈/出栈的 RAII 式对称

每个循环 visit 严格遵循「push -> 编译体 -> 出栈取回」的对称结构。`pop_top` 经 `std::move` 取出栈顶再 `pop`，出栈值复用构造时的同名 `loop_ctx` 变量；回填阶段操作的是独立副本，即使回填过程中 `cur_cu()` 状态变化也不影响 `loop_stack_`。出错路径下（`fail()` 抛 `AriaCompileException` unwind），pop 会被跳过，但整个 `ModuleCtx` 析构时会沿 enclosing_ 链清理 `FunctionCtx`，`loop_stack_` 作为其成员随之销毁，不泄漏。

### 4.4 为什么 `LoopCtx` 是简单聚合而不是带方法的类

`LoopCtx` 的全部行为（入栈、读栈顶、追加占位、回填）都由 `CodeGen` 编排，`LoopCtx` 本身只是数据载体。这与 `FunctionCtx` 的设计分工一致：`FunctionCtx` 负责「登记」（局部/作用域/循环栈管理），`CodeGen` 负责「发射」（`emit_op`/跳转回填/错误）；回填逻辑（`patch_jump`）属于 `CodeUnit` 的编码能力，由 `CodeGen` 调用，不放进 `LoopCtx`。

## 5. 速查：三种循环的 `LoopCtx` 配置

| 循环类型 | `back_target`（回边目标，恒设） | `continue_fwd_patches` | continue 实际跳到 |
| --- | --- | --- | --- |
| `while` | `l_start` | nullopt | 循环头（后向） |
| `for`（有 incr） | `l_cond` | emplace 打开 | `L_incr` 递增区（前向，回填） |
| `for`（无 incr） | `l_cond` | nullopt | 循环头（后向） |
| `for-in` | `l_start` | nullopt | `has_next` 判断处（后向） |

`exit_fwd_patches`（循环头条件假跳占位 + 各 break 占位）三者都用，收尾统一回填到 `L_end`。

## 6. 字段形态的设计依据

跳转由两个端点组成：**src**（发射跳转指令的位置）和 **dst**（跳转目标地址）。谁先确定，就把谁「随身携带」到另一个端点处完成对接。底层三个原语（`CodeUnit` 提供）：

- `emit_jump(op)` -- 发 `op` + 2 字节**占位**，返回占位偏移 src_off。dst 未知时用。
- `patch_jump(src_off)` -- 用「**当前**已发射到的位置」回填占位。前向跳，dst 在后面、此刻才确定时用。
- `emit_jump_back(target_off)` -- 一次性发后向跳，target_off 必须**已经发射过**（已知）。

| dst 在 src 编译时是否已知 | 用法 | LoopCtx 对应字段 | 存的是 |
| --- | --- | --- | --- |
| **已知**（dst 在前面已发射） | `emit_jump_back(dst)` 一条闭环，src 用完即弃 | `back_target`（单值 `u32`） | **dst**（唯一且先知） |
| **未知**（dst 在后面才发射） | `emit_jump` 占位 + 记 src 偏移 + 事后 `patch_jump` | `continue_fwd_patches` / `exit_fwd_patches`（列表） | **src 们**（多个，攒到 dst 处回填） |

字段名也把这个语义写进去了：`back_target` 末尾 `target` = dst；`*_patches` 末尾 `patches` = 待回填的 src 占位偏移。两条推论：

- **没有 `break_back_target`**：break 的 dst 恒为 `L_end`，永远在循环体之后，不可能先确定，所以 break 没有后向分支、没有 dst 字段，只有 src 列表。循环头条件假跳的 dst 同为 `L_end`，故其占位与 break 同入 `exit_fwd_patches`。
- **`loop_scope_depth` 与跳转方向无关**：它只给 `emit_pop_locals_to` 用（§4.2），四种循环都填同一个值（进入循环体前的 `scope_depth_`）。

### 6.1 `for (init; cond; incr) body` -- 唯一的前向 continue

唯一让 continue 走「前向 + 回填」的循环，因为 **continue 必须先执行 `incr` 再回到条件**，而 `incr` 的字节码排在循环体**之后**。编译时间线（↓ 表示发射顺序）：

```
begin_scope()                         // 开 for 自己的作用域，loop_scope = 此刻深度
emit <init>                           // init 局部声明在 loop_scope
L_cond = size()  ◀───────────────┐   记下条件起点
emit <cond>                       │
JUMP_FALSE ──► ? (jf 占位)        │   条件假跳 L_end（前向，后面才知）；src 记入 exit_fwd_patches
                                  │
push LoopCtx{                     │   ┌ back_target          = l_cond（循环头，回边跳此）
  back_target = l_cond,           │   │ continue_fwd_patches = {}（emplace 打开：有 incr 走前向）
  continue_fwd_patches = {},      │   │ loop_scope_depth     = loop_scope
  loop_scope_depth = loop_scope,  │   │ exit_fwd_patches     = {jf 占位}
  exit_fwd_patches = {jf 占位}        │   └
}                                 │
emit <body>  ◄── break/continue 在这里编译：
  │   break:    emit_pop_locals_to(loop_scope); emit_jump(JUMP) -> exit_fwd_patches
  │   continue: emit_pop_locals_to(loop_scope); emit_jump(JUMP) -> continue_fwd_patches
  │                                  │     （有 incr，故走前向占位，记进 fwd_patches）
pop LoopCtx -> loop_ctx            │
                                  │
L_incr = size()  ◀────────────────┘   记下递增区起点 = 前向 continue 的 dst
patch 全部 continue_fwd_patches -> L_incr   ★必须在发射 incr 之前回填★
emit <incr>; POP                      发递增
JUMP_BACK ──► L_cond                   回边，重新判断条件
L_end = size()
emit_loop_backedge_and_exits：回边 -> L_cond；patch 全部 exit_fwd_patches（jf + break 们）-> L_end
end_scope()                           // 弹 init 局部（depth = loop_scope）
```

continue 的 dst 必须是递增区起点 `L_incr`（C 风格 for 的 continue 语义：跳过本轮剩余体，但仍要执行递增再判断），而 `L_incr` 在循环体之后才发射，continue 编译时还不知道 -- 占位 + 回填，这是 `for` 带 increment 独有前向 continue 的全部根因。回填时机约束（「`L_incr = size()` -> 立刻回填 -> 再发 incr」）见 §3 for 条目。

一句话总结：`back_target` 存 dst（单值，因 dst 唯一且先知），`continue_fwd_patches` / `exit_fwd_patches` 存 src 们（列表，因 dst 后知、要把多个 src 攒到 dst 处回填）。`for` 带 increment 是唯一让 continue 的 dst 落在体后的循环。
