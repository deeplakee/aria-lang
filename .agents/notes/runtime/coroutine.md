# 协程实现笔记（单循环切换模型）

> 协程 / 切换 / 跨上下文相关特性重启前必读。机制现状（四原语、状态机、切换原语对）见 `.agents/reference/runtime.md`「协程」，异常侧（寄存器模型、unwind、跟踪行）见 `exception.md`。本文的坑编号自成空间（与 `exception.md` 同号不同坑），引用时带文档名。

## #1 槽协议：预留结果槽住 `call_native_fn` 的 drop

切换型原语返 true 后，`call_native_fn` 事后 `ctx.drop(argc)` 使该上下文栈顶停在「那次调用的槽 0」--这就是预留结果槽，对侧一律写 `对侧->peek(0)`。两侧完全对称，切回方无需知道对方局部信息。

**首启是唯一不对称处**：协程栈只有 `create` 压的 `[closure]`、无预留槽，故首次 resume 走「压实参 + `prepare_call_args` 整形 + `enter_frame` 进帧」。

**纪律**：原语不碰调用者值栈（原生叶子调用契约本就禁止），清理全权归 `call_native_fn`。不能推迟到切回方--它对本上下文当年那次 CALL 的 argc 一无所知，且残槽会跨挂起期滞留值栈。

## #2 置链先于换指（根安全承重）

`coroutine->set_previous(current_)` 必须先于 `current_ = coroutine`。GC 根只标 `current_` 一点，换指后恢复者只经 `co->previous_` 这一条边可达；首启切换后 `prepare_call_args` 的垫充/varargs 打包会经 `new_object` 顶的 `maybe_collect`，恢复者与其实参靠这条边存活（「实参留栈到 drop」是既定红利）。

## #3 一切可失败的检查先于切换

resume 两臂内，元数预检、载荷数上界（已挂起时 `arity_error_range` 1..2）等可失败动作**全部先于** `enter_coroutine`，切换之后只剩不可失败动作。两个原因：失败载荷须落**恢复者**寄存器（`call_native_fn` 的 false 路径契约「禁止 false + 切换」，断言钉）；首启切换后直取 `call_aria_fn` 的整形+进帧尾段（检查段在预检之后全是死分支），不给第二种失败通道。

## #4 状态五态与 `previous_` 判链等价；主上下文的 `state_` 语言不可达

置链/解链只发生在切换原语对内，与状态写点成对，故 resume 的「`state_` 五态穷举 switch」与「`previous_ != nullptr` 判链」在用户可达协程上严格等价--取状态做单源判定，覆盖显式。挂起态 `previous_` 恒空是安全前提（yield 判主上下文、resume 已挂起臂都读它）。

主上下文 uniform 参与换位（resume 置 Normal、让位置 Running），但其 `state_` 无 status 路径、语言不可达，不参与任何判定--不要给它「正确的」初值，也不要读它做分支。

## #5 跨协程错误的四步定序：take -> reset -> leave(Failed) -> raise

1. `take_error()` 先于 `reset()`--寄存器在 `reset()` 内一并清空，取晚了载荷即丢。
2. `reset()` 先关开指再清场--死协程向外泄漏的捕获闭包取值安全。
3. `leave_coroutine(ExecState::Failed)`--置 Failed、解链、恢复者置 Running、换指。
4. `caller->raise(payload)` 收尾--caller 寄存器此刻**必空**，依据三重：`call_native_fn` 进场断言锁「进场前寄存器空」、切换型原生返 true 不写载荷、挂起期间无人能写非执行上下文的寄存器（`raise` 断言防重复）。take 到 raise 之间无 GC 点（reset/leave 零分配），载荷由局部持有不丢根。

中间层无 handler 即连死（其 Running 瞬态被 Failed 覆盖，合法），多跳由 unwind 的循环自然覆盖；调用点不收链参数--命中可能在多跳之后，调用方在循环顶自 `current_` 重取帧（`exception.md` #11 的纪律覆盖此项）。

**预留槽在错误路径无人写**：载荷走恢复者寄存器，resume 的调用区被 `unwind_to_handler` 截栈一并丢弃--不要试图在错误路径补写槽 0。「失败 resume 返回错误值」的形态已被否决（消费时机不存在）。

## #6 跟踪截断在协程边界

死协程的帧在跳链序列随 `reset()` 退场，不并入物化侧跟踪--物化只含物化时刻存活的帧链，最内层 `at` 行 = resume 调用点（原生不进帧，与 `exception.md` #16「原生报错即 caller 帧」同源）。**语料 `.err` 是子串判定，钉不了「协程内帧缺席」**，截断用 C++ 消息精确等值断言。

要做跨边界拼接跟踪（每跳各留 trace 再串）时，须重新评估物化路径「无 GC 分配点」的前提--fn/mod 裸指针跨 `reset()` 存活正靠它。

## #7 死协程不留栈：Done/Failed 一律 `reset()`

完成与未捕获死亡的协程都清空栈，不保留栈快照供事后调试（内存与 sweep 时机保持简单）；代价是拿不到死协程的栈。RETURN 完成切回前先把返回值弹入 C++ 局部再 `reset()`，故返回值不受影响。

## #8 `run()` 的链根锚与出口断言

`run()` 入口锚 `entry_ctx = current_`、出口断言 `current_ == entry_ctx`：dispatch_loop 的出口（顶层 RETURN / 未捕获物化）都发生在链根，漏交接当场炸。`HALT` case 另断言 `current_->previous() == nullptr`（防手写字节码在协程内落 HALT）。主上下文里 `yield` 即运行期错误（`YieldOutsideCoroutine`），挂起不进 `run()` 返回--`run()` 维持两态。

## #9 `create` 免守卫的判据：分配到发布的窗口内零 GC 点

`new_movement` 之后 `co->push(closure)`（Buffer 扩容走 `allocate`/`reallocate`，不触 GC）再到槽 0 发布，窗口内无 GC 点，故免 `make_guard`；发布即随调用者栈根化。一般化：**守卫判据看「窗口内有无 GC 点」，不是「有没有分配对象」**。

## #10 嵌套执行的切换红线：resume 放行、yield 拒绝

重入口落地后，切换型原生在嵌套执行内可达，红线由文档纪律迁入运行期检查，按两侧定向分流：

- **resume 放行**：dispatch_loop 每指令自 `current_` 重取帧，嵌套循环驱动被恢复协程安全；被恢复协程出口边界恒零，完成/让位自动按链切回，嵌套循环随后在自己边界收束。
- **yield 拒绝**：`ret_frame_` 非零即处于嵌套执行，yield 校验首检即报 `YieldInNestedExecution`。嵌套循环等的是同步结果，挂起即穿界--切走 `current_` 后嵌套循环会转去驱动切回方上下文，被挂起上下文的嵌套帧悬在栈上，出口边界语义全部失效。

旧形态的「切换型原生不得在嵌套执行内可达」整体作废（担心外层循环持旧 `frame`/`current_` 驱动复活上下文）；「switch 之后不得新增引用 frame 的代码」同族纪律不受影响。
