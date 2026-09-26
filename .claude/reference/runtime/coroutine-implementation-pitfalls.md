# M6 协程（单循环切换模型）实现坑点记录

> M6 协程通道的坑点与不变式归档，**协程/切换/跨上下文相关特性重启前必读**。设计基线见
> `vm-design.md` §4.9，机制现状见 `.claude/rules/runtime.md`「协程」节，异常侧背景（寄存器
> 模型/unwind/跟踪行）见 `exception-implementation-pitfalls.md`（坑编号 #1-#20，本文沿用
> 其「坑 #N」编号空间之外的独立编号）。落地批次：B1 对象化（96aa55e/bc93d79/d0ac244/12bee08）、
> B2 状态机与四原语（8adae5c/5b3151a/e1508fc）、B3 跨协程错误（310a20c）。

---

## 坑 #1：槽协议 -- 预留结果槽住 call_native 的 drop，不在原语里写调用者栈

**机制**：切换型原语返 true 后，`call_native` 事后 `entered_ctx->drop(argc)` 使该上下文栈顶
停在「那次调用的槽 0」-- 这就是预留结果槽，对侧一律写 `对侧->peek(0)`。两侧完全对称，切回方
无需知道对方局部信息。**首启是唯一不对称处**：协程栈只有 create 压的 `[closure]`，无预留槽，
故首次 resume 走「压实参 + `prepare_call_args` 整形 + `enter_frame` 进帧」。

**纪律**：原语不碰调用者值栈（原生叶子调用契约本就禁止），调用者侧收敛全权归 call_native；
不能推迟到切回方清理--切回方对本上下文当年那次 CALL 的 argc 一无所知，且残槽会跨挂起期滞留
值栈。

## 坑 #2：置链先于换指（根安全承重，住 enter_coroutine 注释）

**约束**：`coroutine->set_previous(current_)` 必须先于 `current_ = coroutine`。GC 根只标
`current_` 一点，换指后恢复者只经 `co->previous_` 这一条边可达；首启切换后
`prepare_call_args` 的默认垫充/varargs 打包经 `new_object` 顶 `maybe_collect`，恢复者与其实参
以此边存活（「实参留栈到 drop」是 call_native 的既定红利，实参仍在恢复者调用区为根）。

## 坑 #3：一切可失败的检查先于切换

**约束**：resume 两臂内，元数预检（首启交 `check_arity` 报被调者元数）、载荷数上界（已挂起
`arity_error_range` 1..2）等可失败动作全部先于 `enter_coroutine`；切换之后只剩不可失败的
动作。原因有二：失败载荷须落**恢复者**寄存器（call_native 的 false 路径契约「禁止 false +
切换」，断言钉）；首启直取 `call_closure` 的整形+进帧尾段（检查段在预检后全为死分支），
不给第二种失败通道。

## 坑 #4：状态五态与 previous_ 判链等价；主上下文 state_ 语言不可达

**事实**：置链/解链只发生在切换原语对内（enter 置链、leave 解链），与状态写点成对，故
resume 的「`state_` 五态穷举 switch」与「`previous_ != nullptr` 判链」在用户可达协程上严格
等价--取状态单源判定因覆盖显式（2026-09-26 裁定）。挂起态 `previous_` 恒空是安全前提
（yield 判主上下文、resume 已挂起臂都读它）。主上下文 uniform 参与换位（resume 置 Normal、
让位置 Running）但其 `state_` 无 status 路径、语言不可达，不参与任何判定--不要试图给它
「正确的」初值或读它做分支。

## 坑 #5：跨协程错误的四步定序 -- take -> reset -> leave(Failed) -> raise

**定序承重**（unwind 的转投一跳）：

1. `take_error()` 先于 `reset()` -- 寄存器在 reset 内一并清空，取晚了载荷即丢。
2. `reset()` 先关开指再清场 -- 死协程向外泄漏的捕获闭包取值安全（复用既有行为）。
3. `leave_coroutine(ExecState::Failed)` -- 置 Failed、解链、恢复者置 Running、换指。
4. `caller->raise(payload)` 收尾 -- caller 寄存器此刻**必空**，依据三重：call_native 进场
   断言锁「进场前寄存器空」、切换型原生返 true 不写载荷、挂起期间无人可写非执行上下文的
   寄存器（`raise` 断言防重复 raise）。take 到 raise 之间无 GC 点（reset/leave 均零分配），
   载荷局部持有不丢根。

中间层无 handler 即连死（其 Running 瞬态被 Failed 覆盖，合法），多跳由 unwind 的 while
自然覆盖。调用点（dispatch_loop 尾 `unwind_check` 标签）一字不改、unwind 不收链参数--
命中可能在多跳之后，调用方经循环顶自 `current_` 重取帧，M3 坑 #11 的既有纪律正好覆盖。

**预留槽在错误路径无人写**：载荷走恢复者寄存器，resume 的调用区被 unwind_to_handler 截栈
一并丢弃--不要试图在错误路径补写槽 0，「失败 resume 返回错误值」的形态已被否决（消费时机
不存在，见 vm-design.md §4.9「错误跨协程」的被否记录）。

## 坑 #6：跟踪截断（P4）-- trace 每跳重新收集，截断在协程边界

**约束**：unwind 的 `trace` 是跳链循环的逐跳局部，死在边界的协程帧不并入物化侧的跟踪--
物化只含最终命中/物化那一层的帧链，最内层 at 行 = resume 调用点（原生不进帧，与
exception-pitfalls 坑 #16「原生报错即 caller 帧」同源）。**语料 `.err` 是子串判定，钉不了
「协程内帧缺席」**，截断用 C++ 消息精确等值断言钉（`UncaughtInCoroutineTraceTruncatedAtBoundary`）。
将来若要跨边界拼接跟踪（每跳保留各自 trace 再串），须重新评估物化路径「无 GC 分配点」的
前提（fn/mod 裸指针跨 reset 存活正靠它）。

## 坑 #7：死协程不留栈 -- Done/Failed 一律 reset

**裁定**（Lua 风格）：完成与未捕获死亡的协程 `reset()` 清空栈，不保留栈快照供事后调试，
内存与 sweep 时机保持简单；代价是拿不到死协程的栈（将来要调试面再议）。reset 先
`close_upvalues` 再清场，死协程向外泄漏的捕获闭包取值安全。RETURN 完成切回前先弹返回值入
C++ 局部再 reset（`ret` 不殃及）。

## 坑 #8：run() 链根锚与出口断言 -- 漏交接当场炸

**机制**：`run()` 入口锚 `entry_ctx = current_`、出口断言 `current_ == entry_ctx`。
dispatch_loop 的出口（顶层 RETURN / 未捕获物化）都发生在 resume 链链根 = 入口上下文，
漏交接当场炸、不等下一轮入口。HALT case 另断言 `current_->previous() == nullptr`
（真实程序不产 HALT，防手写字节码在协程内落 HALT 的形态漏网）。主上下文里 yield 即
运行期错误（YieldOutsideCoroutine），挂起不进 run() 返回 -- run() 维持两态，
设计初稿的 ExecOutcome 三态退役。

## 坑 #9：create 免守卫的判据 -- 分配到发布的窗口内零 GC 点

**判据**：`new_movement` 之后 `co->push(closure)`（Buffer 扩容不触 GC，allocate/reallocate
不变式）再到 `slots[0]` 发布，窗口内无 GC 点，免 make_guard；发布即随调用者栈根化。
一般化：守卫判据看「窗口内有无 GC 点」，不是「有没有分配对象」。

## 坑 #10：嵌套 run_closure 红线 -- 切换型原生不得在嵌套 run_closure 内可达（成文不变式）

**约束**：`run_closure` 是 AriaVM 成文的「未来重入接缝」。当前零原生回调用户闭包
（run_closure 仅 run() 一个调用方，IMPORT 走同循环不嵌套），yield 不可能落进嵌套循环；
**将来任何原生若同步调用用户闭包（getter 一类），必须保证切换型原生（coroutine.resume/
yield）在该嵌套执行内不可达** -- yield 在嵌套 dispatch_loop 内切换会让复活上下文被错误的
C++ 循环帧驱动（外层循环持着旧的 frame/current_ 局部）。同族防线是 dispatch_loop 的
「switch 之后不得新增引用 frame 的代码」纪律。重启 getter/回调类特性时先对照此条。

---

## 验收面索引

- 语料：`tests/language/positive/14_coroutines/`（含 error_hop_to_resumer / error_hop_multi_level
  三上下文连死与中间层自救 / status_projection 五态投影）；负向
  `runtime_uncaught_error_in_coroutine`（跟踪截断 .err）、`runtime_resume_non_suspended_coroutine`
  （在链上，只能发生在协程内）、`runtime_resume_failed_coroutine`（Failed 与 Done 同码同文案）。
- C++：`UncaughtInCoroutineTraceTruncatedAtBoundary`（截断精确等值断言）、
  `SuspendedCoroutineValuesSurviveStressGc` / `InterleavedCoroutinesSurviveStressGc`
  （stress GC 悬挂压力）。
