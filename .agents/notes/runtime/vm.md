# VM 结构与被否方案

> 现状（状态分层、值栈/帧栈、切换协议、异常通道、原生调用约定、执行入口）见 `.agents/reference/runtime.md`；异常定序见 `exception.md`；协程定序见 `coroutine.md`。本文只收两样东西：**改 VM 必须守的红线**，与**已经试过/想过、不要再重提的方案**。

## 1. 状态分层

VM 的状态按「谁拥有」切两层，判据是「整进程一份，还是每协程一份」：

| 层 | 状态 |
| :--- | :--- |
| 解释器级（`AriaVM`） | GC、模块表、内置函数注册、bootstrap 类格、`current_`（唯一协程根）、源根 |
| 执行级（`AriaExeContext`） | 值栈、帧栈、open upvalue 链、执行状态机、挂起错误寄存器、嵌套执行边界与重入深度、`previous_` |

**主上下文与协程同构**：主上下文就是「永不 yield 的协程」，`run()` 对二者零特判。协程要当一等值（存 list、传参），故最终形态必须是 `Object` 子类型；主上下文为此多付一个对象头（几十字节），相对值栈（KB 量级）可忽略。

驱动代码独立成 eval 层（`class Eval` 全静态、AriaVM 友元缝），AriaVM 收窄为宿主。**签名即上下文**：ctx 耦合的机器随上下文走（`dispatch_loop` 体内在换指点后重取本轮指针），只有要跨协程跳链的 `unwind` 恒持 `AriaVM&`。

## 2. 硬红线

**值栈可增长 → 三类指针必须重绑**。`push` 溢出时 2x 增长、整体搬迁（GC reallocate）。增长打破「指针绝对稳定」，故每次增长后指进值栈的裸指针全部重定位：`top_`、各活动帧 `slots`、open upvalue 链的 `location_`。前两类走栈内定长数组；第三类节点数不定，必须**偏移两趟**--搬运前走链把 `location_ - old_base` 记进 `List<usize>`（链序两趟间稳定），搬运后 `new_base + 偏移` 重建。对 dangling 指针做指针减法是 UB，所以偏移只能在搬运前记。

**`ip` 用裸指针，offset 冷路径换算**。热路径 `*ip++` 取操作数；`find_try_handler(u32)` 与行号表吃 offset，但都在 raise/报错这样的冷路径，一次减法换算即可。

**帧引用可缓存于循环外**，因为 `FrameStack` 永不扩容。但任何会换指（换 `current_`）或弹帧的调用点之后，必须重新取 `frames().top()`--循环顶每轮断言 `frame == &current->frames().top()` 就是钉这条。

**派发恒为单 switch**。不要引入 computed goto（见 §3）。

**错误通道**：raise 与 unwind 不融合成 `*_and_*` 助手，站点就地两步；`unwind` 只做纯搜索，不动帧栈/值栈、不物化、不收集跟踪；物化与跟踪烘焙都在 `rebake_uncaught_error()`。

**原生函数是叶子**：不得操作值栈簿记、实参只读、返回值经槽 0 就地写（`drop(argc)` 后槽 0 升顶）；`return false` ⟺ 已置寄存器，二者由 `return ctx.fail(...)` 自动满足。视图按下标（不是指针）持槽 0，重入扩容搬迁也不会让实参读数悬垂。

**运行期位置只归未捕获出口的 `at` 行**：消息不含位置前缀，装箱也不烘位置；`last_ip` 一处写（循环顶）同时服务查表与行号。

**不设 `SETUP_EXCEPT`/`END_EXCEPT`**：try 范围与 handler 走 CodeUnit 记录表。

## 3. 已否决的方案（勿重提，除非测量条件变了）

**值栈复用 `FrameStack<Value, N>`**。帧栈是 acquire/pop/truncate 的槽位语义，值栈热路径要 push/peek(k)/`Value*` 寻址（callee 的参数就躺在栈顶、直接当它的 slots 区）。语义不重合，故值栈自持 GC 分配的 `Buffer<Value>` + `top_`。

**索引式 upvalue**（存槽下标而非指针，免重绑）。省下重绑，但每次 `LOAD/STORE_UPVALUE` 多一次加法，且偏离 clox/Wren/Lua 的指针式模型。取指针式 + 三类重绑。

**computed goto 派发**。曾以双形态落地过（标签地址跳表 + `TARGET`/`DISPATCH` 宏），实测弃用：clang 会把全部派发尾合并成共享 trampoline，per-site 间接分支在二进制里根本不存在，而 indirectbr 的 CFG 语义又迫使优化器在共享派发点做全状态广播，代码体量与每派发指令数双增、显著更慢；GCC 能兑现 per-site 且无广播成本，但其 switch 被编成比较链、基线本身弱，语言级收益微小且浮点重型负载上大幅回退。CPython/Lua 保留 computed goto 的前提是主场 GCC 上有两位数收益，aria 不存在这个前提。

**「整个 `dispatch_loop` 生命期的入口快照」**（把宿主/上下文指针缓存进局部、整轮复用）。正确形态只能是**指令级工作副本 + 显式同步点**（Wren 的 `STORE_FRAME`/`LOAD_FRAME` 那套）；循环顶每轮重绑引用实测更慢（每轮多几次内存载入换不到收益），不要再做。全堆驻留换来的是切换零同步成本。

**`run()` 三态返回值**。主上下文里 yield 是运行期错误（`YieldOutsideCoroutine`），挂起只发生在协程内，`run()` 出口恒为顶层 RETURN 或未捕获物化，两态即终态。

**`main_ctx_` 作值成员**。主上下文一并堆化（ctor 首笔 GC 分配）换来 tracer 只标 `current_` 一点 + 主/协程类型统一。

**「失败 resume 返回错误值」**。resume 切走时 `ctx.drop(argc)` 已执行、`return true` 已返回，CALL 善后点早已收工，主循环没有逐指令 `has_error()` 检查，恢复者侧没有任何时机消费该载荷。正确形态是 unwind 自身沿 resume 链跳。

**在装箱点烘 `path:line` 前缀**。与未捕获跟踪行重复，且用户 `throw` 原值与透传编译错两路本就无/自带位置。

**按模块预填 globals 注册内置**（会在 REPL 逐行 `run()` 时重注册、覆写用户 shadow）。取「VM 级只读 builtins 表 + `LOAD_GLOBAL` miss 回退」：一份表共用，省每模块若干 `ObjFunction` 分配。

## 4. 外部参照

协程模型对标 Wren 0.4 fiber（`runInterpreter` / `runFiber` / `fiber_yield` / `blackenFiber`）：单循环显式切换、原语而非操作码、`previous_` 对齐 caller 语义。aria 的差异是**零同步成本**--解释器状态全堆驻留（`ip` 在 `CallFrame`、栈顶在 ctx、每指令重取帧），没有可陈旧的循环级局部副本，故不需要 `STORE_FRAME`/`LOAD_FRAME` 那对宏。
