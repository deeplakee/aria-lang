# 异常实现笔记（`try/catch/throw` 与运行期错误）

> 异常类特性重启前必读。机制现状（unwind 三分流、物化点、跟踪行）见 `.agents/reference/runtime.md`「VM 异常通道」，发射侧见 `compile.md` 与 `CodeGen::visitTryStmtNode`，记录表见 `bytecode.md`。本文只收代码注释里读不到的定序与不变式。

**编号是历史锚点，被 `tests/` 按号引用，不得重排或复用。** 空号（#9、#12）与已并入别条的号（#5 并入 #6/#13、#14 并入 #7）保持空缺，别补号。

## 1. 定位锚：`last_ip` 是「指令起始」，不是 `ip`（#1、#2）

- `find_try_handler` 用 `[begin, end)` 半开区间匹配，而 raise 时刻 `frame.ip` 已推过当前指令的操作数。若 THROW / 除零是 try 体**最后一条**指令，`ip == end` 落在区间外，本应被捕获的错变成未捕获。
- 故 raise 时用 **`frame.last_ip`（指令起始）** 反推 offset。每轮取指前无条件写 `frame.last_ip = frame.ip`（`dispatch_loop` 循环顶），单次指针 store 换对所有 raise 站点的统一正确性，无需 `ip_start` 快照、无需站点自带偏移。
- 跨帧同理：CALL 进帧后调用者帧的 `ip` 停在 CALL 之后，要查的是 **CALL 站点**是否落在 try 区间内--而 `last_ip` 就停在 CALL 站点。IMPORT 模块体调用同形。所以 `unwind` 统一读 `frame.last_ip`，不区分顶帧/外帧。
- 帧内**存指针**（与 `ip` 同取向：热路径裸指针、offset 冷路径换算），前提是帧存活期间 `code` 缓冲恒定（非移动 GC + 执行期零 emit）。

## 2. `CallFrame` 不得有 NSDMI（#3）

`FrameStack` 要求 `T` trivial + trivially-copyable + trivially-destructible（static_assert 三连）。默认成员初始化会把默认构造变非 trivial，直接炸。新字段初始化收敛进 `CallFrame::init`，注意 `last_ip` 置 `= ip`（code 起始）而非 `nullptr`（与 `data()` 相减是 UB）。

## 3. `try_records` 按 `begin` 非降序：入口预插占位 + 结尾回填（#4）

单遍编译下嵌套 try 会**降序** push（内层先编译完先 push，而它的 `begin` 更大）。解法是 `visitTryStmtNode` **入口**就预插占位（`begin` 已知，`end/handle/stack_depth` 先置 0），拿到 `rec_idx`；体内嵌套 try 各自预插，`begin` 更大、自然排在其后；结尾回填占位项。构造即升序，不需要编译末尾 sort。`stack_depth` 也在入口快照（try 体 `begin_scope` 之前的 `locals_.size()`）。

`find_try_handler` 的二分是「先求首个 `begin > ip`，再前溯找首个 `end > ip`」= begin 最大的覆盖区间 = 最内层。前提是区间**良嵌套**（不交叉）。

## 4. `TryRecord` 不存帧深度，只存 `stack_depth`（#5）

帧深度是运行期量，编译期不可知，由「自最内帧向外遍历、首命中即在该帧 unwind」隐式决定。`TryRecord` 只有 `{begin, end, handle, stack_depth}` 四字段。

## 5. `stack_depth` 相对 `frame.slots`，不是栈基址（#6）

它是「try 入作用域时**本帧**已声明的局部数」。截栈基准 = `handler_frame.slots + stack_depth`：丢弃 try 体内临时值与本帧残留，保留 handler 帧的 callee/参数/已声明局部。动作住 `AriaExeContext::unwind_to_handler`（弃帧 + 关开指 + 截栈 + 跳 handler + 载荷落槽一体）。

## 6. 单寄存器 `pending_error_ : Opt<Value>`（#7、#14）

两个侧面决定载荷形态：catch 侧要绑 **aria 值保类型**（`throw 42` -> `e` 是 Int 42），`run()` 侧未捕获要回**结构化 `Error` 保码**。单 `Opt<Error>` 丢类型，单 `Opt<Value>` 丢码，故：单寄存器 `Opt<Value>`，**运行期错误包成 `ObjException`（携码 + 烘焙消息）**、**用户 `throw` 存原值**。

- 收益：**re-throw 保码**。`try { 1/0 } catch (e) { throw e }` 未捕获时仍是 `DivisionByZero`，而非退化成 `UncaughtException`。
- 分工：`AriaExeContext::raise(code, detail)` / `fail` 负责构造 `ObjException` 并存 `Value`；只暴露 `raise(Value)`（存原值、不包）、`has_error()`、`take_error()`、只读 `pending_error()`。寄存器单槽且 `raise` 在已有挂起时 ASSERT，故必须先 `take_error()`；`reset()` 一并清空。
- 消息**不含位置前缀**（位置归未捕获出口的 at 行，见 §11）；编译期透传的错误自带位置、原样保留。`Error::from_baked` 是「装已烘串」的唯一入口（跳过 `make_message` 的双重前缀），合法调用方只有 `ObjException::to_error()` 与 `AriaVM::rebake_uncaught_error()`。

## 7. 挂起寄存器不在值栈内，必须显式标根（#8）

raise 到 take 之间可能跨 `new_exception` 的 `maybe_collect` 与 handler 处截栈/push 触发的 GC；寄存器里的 `ObjException`（及用户 throw 的任意 `Obj*`）不在值栈区间，若不标根就被回收、catch 绑到悬垂对象。归属：`ObjMovement::trace` 自标 `pending_error_`（VM 根 tracer 只标 `current_` 一点，其余经对象图级联）。try/catch 路径必须开 stress GC 验证。

## 8. catch 参数走「值填槽」：`catch_slot == stack_depth`（#10）

`stack_depth` = try 入口活局部数 = 下一可用槽；try 体局部在 `end_scope` 弹回，catch 参数正是下一个 `add_local`，故 **catch 参数槽恒等于 `stack_depth`**。`unwind_to_handler` 截栈后 `push(载荷)` 恰好落在该槽--handler 不发 `STORE_LOCAL` 前导，`TryRecord` 也不存 `catch_slot`（存了是自赋值 no-op）。`catch_slot` 只编译期用（catch 体读 `e`）。

**栈平衡**：`e` 必须收进 catch 子句 scope（包住 `e` + catch 体），让两条路径在 `L_end` 栈高齐平（正常路径 try 体 `end_scope` 弹至 `stack_depth`；异常路径 catch 子句 `end_scope` 弹回 `stack_depth`）。

## 9. 命中 handler 后必须回循环顶重取帧（#11）

`dispatch_loop` 循环顶持 `frame` 引用；`unwind` 命中时 `frames_.truncate` 可能弹掉多帧，旧引用立即悬垂。现行形态：错误站点 `goto unwind_check`，命中即回循环尾、回循环顶重取帧；换指后需重取上下文/帧的站点 `goto sync_now`。**switch 之后与两个标签体内都不得新增引用 `frame` 的代码**（该前提钉在循环顶注释），循环顶每轮断言 `frame == &current->frames().top()`。

## 10. `unwind` 是纯搜索，不动帧栈（#13）

遍历只读各帧 `last_ip` 查表；命中后交给 `unwind_to_handler` 一体完成，未命中的帧不被逐个弹出（早期形态逐帧 `exit_frame` 有两处硬伤：帧引用悬垂；走到未捕获时帧已全弹、跟踪无帧可查）。未命中按出口边界三分流：嵌套执行截净残帧交外层续搜 / 协程链转投一跳 / 链根返 false 交 `run()` 物化。

## 11. 位置只归未捕获跟踪行（#15）

运行期错误的位置**不烘进** `ObjException`，唯一载体是未捕获出口的 `at` 行。顶帧 `last_ip` = 故障指令，`call_*` 失败与原生报错 = caller 帧的 CALL 站点（原生不进帧），故无需在 take 点补标。早期「装箱点烘 `path:line` 前缀」及配套助手已删（与跟踪行重复）。

## 12. 跟踪在物化点收集（#16）

跟踪**不存 `ObjException`**（对标 Python：traceback 取自活帧，异常对象不背全程 trace），在 `rebake_uncaught_error()` 一次性生成：现场自顶帧向下收集 `(proto, module, ip_off)`，按内 -> 外逐帧烘 `  at <fn名> (<loc>)` 追加到 `Error::message_` 尾部（烘进消息而非直接输出：`interpret_run` 零改动、测试可断言、嵌入方可自行展示）。物化只含物化时刻存活的帧链--死协程帧已随转投 `reset()` 退场、嵌套执行残帧已被边界臂截净，故跟踪截断在协程边界，最内 `at` 行 = resume 调用点。

## 13. 命中分支三步定序：close -> 截栈 -> push（#17）

try 体里被闭包捕获的局部，其 upvalue 开指着本帧槽；命中 handler 时**帧不退**（不经 `exit_frame`），丢弃的只有值栈区间 `[slots + stack_depth, top)`，必须显式关该区间开指。

顺序承重：载荷 push 落在 catch 参数槽 == slot `stack_depth` == **弹弃区首槽**，若不先关，push 会覆写被捕获局部槽、迁值读到的是异常值。截栈本身只移 `top_` 不覆写，首个覆写点就是 push，故 `close_upvalues(catch 槽)` 必须在它之前（三步都住 `unwind_to_handler`）。

## 14. `exit_frame` 内置关闭 + `reset()` 安全网（#18）

「帧退出 ⇒ 本帧区间开指全关」做成**单点结构保证**：`close_upvalues(frame.slots)` 内置进 `exit_frame`，与弹帧、栈顶复位一体，调用方无法只做其一。两个细节：先取 `slots` 再 close/pop（`frames_.top()` 在 pop 后悬垂）；关闭须在槽区仍存活时完成（弹帧后调用者的 push 会自 callee 槽起覆写已弃区）。

**`reset()` 安全网**：HALT 收场不弹帧，开指残留会在 `run()` 复用主上下文时跨 run 指入被覆写的栈区，再经幸存闭包读出脏值。故 `reset()` 先 `close_upvalues(buf_.data())`（全链）再清场；`run()` 前后各一次即激活此网。注意 `reset()` 不碰 `previous_`/`state_`--协程死亡路径依赖这一豁免。

## 15. `CLOSE_UPVALUE` 批量关闭的三个安全前提（#19）

作用域退出**不弹栈**（对齐 Lua `OP_CLOSE`）：`POP_N` 整区弹局部（被捕获局部一并计数），弹区含被捕获局部才追加一条 `CLOSE_UPVALUE`（关闭槽址 >= 新栈顶的开 upvalue）。此时弹区槽已在新栈顶之上，close 仍要读它们迁值，安全性靠三条：① 两指令均无分配、无 GC 点，其间无覆写触发点；② close 只读弹区不写弹区（迁值写各自 `closed_`）；③ 外层帧槽址恒低于本帧，故「槽址 >= 本帧新栈顶」只属弹区局部。

**若在 `POP_N` 与 `CLOSE_UPVALUE` 之间插入任何 push（调试钩子一类机制），前提 ① 即破**--插入任何 push 类机制前必须重新核对这一条。

for-in per-iteration 出口每轮都发（若捕获），即「每轮新鲜绑定」的落地机制。

## 16. 开 upvalue 链的 GC 标根与值栈增长重绑（#20）

- **GC 悬垂（clox 已知坑）**：链上节点可能仅被链本身引用（闭包已死而 upvalue 未关），不标就回收、链上留悬垂，后续 `close_upvalues`/`capture_upvalue` 走链即 use-after-free。归属 `ObjMovement::trace`（沿链逐节点标根）。
- **值栈增长重绑（第三类指针）**：`grow_stack_` 搬运后 `location_` 成 dangling，与 `top_`/各帧 `slots` 同法**偏移两趟**--搬运前走链把各 `location_ - old_base` 记入 `List<usize>`（节点数不定、不能用栈内定长数组；链序两趟间稳定），搬运后 `new_base + 偏移` 重建。对 dangling 指针做减法是 UB，故偏移必须搬运前记。

## 17. `finally` 与 defer

`finally` 已从文法移除（`finally` 回归普通标识符，try 必须带 catch，报 `TryWithoutHandler`），故 unwind 流程**没有 finally 汇合点**。善后 defer 已裁定不做，若重开它与本机制共享两个难点--① unwind 途中执行用户代码；② 善后代码自身抛错会替换在途异常，且在途异常不能只住单寄存器（途中嵌套 try/catch 命中会清掉它），须另存专用槽位。若重新引入 finally，控制流语义从零定（不继承 Java/Python 的覆盖语义）。
