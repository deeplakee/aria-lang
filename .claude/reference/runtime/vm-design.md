# VM 设计与实施计划

本文档规划 `AriaVM` 与执行上下文的设计,并给出分阶段实施路线。基准:`bytecode-instruction-set.md`(指令集/栈效应/操作数编码)、`FrameStack.hpp`、`CodeUnit.hpp`(含 `TryRecord`/`find_try_handler`)。

> **实施原则:先让虚拟机跑起来。** 完整的协程/GC 根/闭包等机制按阶段推进(见 §6),早期阶段(M1)刻意收敛到最小,不在第一步背上全部设计复杂度。

## 1. 命名决定

- 协程/执行上下文实体类名:**`ObjMovement`**(`Object` 子类型,M6 已自纯 C++ 类 `Movement` 升级而来)。
- **`VMContext` 是 `ObjMovement` 的别名**:`using VMContext = ObjMovement;`。代码中按语义选用--泛指「一段执行的状态」时用 `VMContext`,强调「协程对象」时用 `ObjMovement`。
- `ObjType::MOVEMENT` 已增补(语言可见名 `Movement`,`type(co)` 报之)。
- M1 先以纯 C++ 类 `Movement` 落地(不继承 Object);M6 升级为 `ObjMovement : Object` 仅重命名 + 加 trace,别名的存在使调用方代码零改动。

## 2. 状态分层

VM 的全部状态按「谁拥有」切两层,`VMContext` 的引入点即在此:

| 层 | 状态 | 特征 |
| :--- | :--- | :--- |
| **解释器级(AriaVM,共享)** | GC、模块表(每模块自己的 globals)、内置函数注册、Object 根类 bootstrap、`current_`(正在运行的上下文,唯一 VM 级协程根;其余协程经对象图可达,§4.9) | 整个进程一份,所有协程共享 |
| **执行级(VMContext,每协程一份)** | 值栈、`FrameStack<CallFrame> frames_`、open upvalue 链头、执行状态机、挂起错误寄存器(侧信道,见 §4.7)、(协程期)resume 链 `previous_` | 协程的实体就是这堆状态 |

核心结论:**主上下文与协程同构**--主上下文就是「永不 yield 的协程」,`run()` 循环对二者零特判。协程要是一等值(可存入 list、可传参),故最终形态必须是 `Object` 子类型;主上下文作为 Object 多付的只是对象头(几十字节),相对值栈(约 KB)可忽略。

## 3. 结构草图

```cpp
// ---- 执行级 ----
class ObjMovement /* final : public Object, M6 起继承 */ {
    // 值栈:初始定容、push 溢出时 2x 增长(搬迁时重定位 top_ 与活动帧 slots,见 §4.1)
    Value*        stack_;       // GC 分配,初始 kStackInit 个 Value,可增长
    Value*        top_;         // 栈顶指针,热路径裸指针操作
    FrameStack<CallFrame, kFrameMax> frames_;
    ObjUpvalue*   open_upvalues_;           // 开指 upvalue 链(M4 已落地)
    ExecState     state_;                   // M6:五态 Suspended/Normal/Running/Done/Failed(§4.9)
    ObjMovement*  previous_;                // M6:resume 链(挂起态恒空;运行链指向恢复者)
    // M6 协程期字段即上两行;初始化收敛进 ctor(复杂类纪律,无 NSDMI)
};

// 帧是纯 POD(FrameStack 要求 trivially-copyable;只持指针,满足约束)
struct CallFrame {
    ObjClosure*  closure;    // M4 起持闭包(callable 收敛为闭包,顶层入口也是闭包);元数据经其 function() 取
    CodeUnit*    unit;      // 缓存 closure->function()->unit(),省每条指令一跳
    ObjModule*   module;    // 缓存 closure->function()->module(),供 *_GLOBAL 定位模块 globals
    u8*          ip;        // 裸指针最快;raise 等冷路径按需算 offset
    Value*       slots;     // 本帧局部基址(普通帧 callee=槽0、方法帧 this=槽0(M5),参数从槽 1 起、局部)
    u8*          last_ip;   // 最近取指指令起始(行号/unwind 查表锚点)
};

// ---- 解释器级 ----
class AriaVM {
    GC            gc_;           // VM 拥有 GC 值成员(已定:每 VM 一个 GC)
    AriaHashTable modules_;      // 模块表(键=规范路径 ObjString*、值=ObjModule*,均装箱 Value)
    AriaHashTable builtins_;     // VM 级只读内建表(LOAD_GLOBAL 模块 globals 未命中回退查此,§7)
    List<Object*> registers_;    // 值寄存器组(VM 单例对象统一存放表;Object 根类在 ObjectClass 格,不进任何名字空间;GC tracer 逐格标根,§4.6)
    ObjMovement*  current_;      // 当前执行上下文 = 唯一上下文成员(ctor 首笔 GC 分配,主上下文即其初值,无独立 main_ctx_;M6 单循环切换:resume/yield 原生函数在 call_native 善后换指/回退,§4.9;链根交接由 run() 入口锚 + 出口断言钉)
    List<String>  source_roots_; // 源根列表([0]=入口根、[1..]=配置根)
    // (M6 定稿:无需 contexts_ 调度列表 -- 协程即 Object,经用户持有的协程值 + current_ 单根可达,§4.9)

    Result<Value, Error> run();  // 驱动至顶层 RETURN / 未捕获物化(两态即终态,见下注)
    void        raise(ErrorCode, StringView detail); // 装箱入挂起寄存器,一步烘齐(§4.8);失败信号惯用法由 fail 承载(哨兵按调用点返回类型转 false/nullptr/nullopt,`return vm.fail(...)` 一行)
    bool        call_value(Value callee, u8 argc); // CALL 与嵌入 API 共用;callee 分发闭包/原生/ObjClass(实例化)/ObjBoundMethod(M5);bool 成败信号,失败载荷在挂起寄存器(§4.7)
};

// run() 维持两态 Result<Value, Error>:主上下文里 yield 是运行期错误(YieldOutsideCoroutine),
// 挂起只发生在协程内,run() 的出口恒为顶层 RETURN 或未捕获物化、都发生在 resume 链链根,
// 无第三态可报(设计初稿的 ExecOutcome 三态退役)。
```

协程采用**单循环切换模型**(M6 定稿并落地,详见 §4.9):`resume`/`yield` 是原生函数,换走 `current_` 后返回,CALL 分支善后点发现切换、回到循环顶,循环自然开始驱动新上下文--**不重入 `dispatch_loop()`**,任一时刻整个 VM 只有一个循环在执行。主上下文里 yield 即运行期错误,挂起只发生在协程内,`run()` 的出口恒为顶层 RETURN / 未捕获物化、且必发生在 resume 链链根(run() 入口锚 + 出口断言钉住切换交接的完整)。

## 4. 关键决策

### 4.1 值栈可增长,搬迁时重定位指针

值栈初始定容(`kStackInit`)、`push` 溢出时 2x 增长:整体搬迁(GC reallocate,内部 memcpy)后,把 `top_` 与所有活动帧的 `slots` 按新旧基址差重定位。增长打破「指针绝对稳定」,故每次增长后指进值栈的裸指针都必须重定位--M4 起共**三类**:`top_`、各活动帧 `slots`、open upvalue 链的 `location_`(M4 已落地第三类:搬运前走链把各 `location_` 相对 old_base 的偏移记入 `List<usize>`(链序两趟间稳定,免数节点一趟),搬运后 `new_base + 偏移`重建;同法偏移两趟、从不触碰 dangling 指针,见 `Movement::grow_stack_`。指针式 upvalue 表示的既定取舍--「索引式免重绑」方案被否:它省下重绑,但每次 `LOAD/STORE_UPVALUE` 多一次加法,且偏离 clox/Wren/Lua 与本文 §4.1/指令集 §4.4 预写的指针式模型,不取)。帧栈 `FrameStack` 仍一次分配永不扩容:帧数少、无需增长,且其指针稳定性不受值栈搬迁影响。

### 4.2 值栈不复用 `FrameStack<Value, N>`

帧栈是「acquire/pop/truncate」的槽位语义;值栈热路径需要 `push/peek(k)/Value* 寻址`(callee 的参数就躺在栈顶,直接作为其 slots 区)。语义不重合,`Movement` 自持 GC 分配的 `Buffer<Value>` 底座 + `top_` 裸指针管理(经 GC 分配/重分配,计入 `bytes_allocated_`)。容量参考:值栈初始 `kStackInit` = 1024 个 Value(8KB)、`push` 溢出 2x 增长;`kFrameMax` = 256(帧栈仍定容)。每上下文初始内存约 8KB 量级,数千协程无压力。

### 4.3 `ip` 用裸指针,offset 按需换算

热路径 `*ip_++` 取操作数;`find_try_handler(u32)`/行号表吃 offset,但都在冷路径(raise/报错),`offset = ip - unit->code.data()` 一次减法。

### 4.4 帧引用可缓存于循环外

`FrameStack` 永不扩容 => `CallFrame&` 引用整轮循环稳定,可像 clox 一样把 `frame`/`slots` 缓存进局部,CALL/RETURN 时刷新。

### 4.5 异常衔接(与 AGENTS.md「错误处理」第 2 条一致)

- **内部传播统一走寄存器 + unwind**：op 处理局部失败不再直接 `return runtime_err(...)` 短路出 dispatch_loop,而是就地 `raise`(一步烘消息装成 ObjException,不含位置前缀 -- 位置归未捕获跟踪行,见 §4.8)-> 存当前上下文挂起寄存器后调 `unwind` 查表派发 -- raise 与 unwind 不融合成 `*_and_*` 具名助手,站点两步(raise / run_* 返 false 后显式 `goto`)、与 CALL 失败善后同形,检查(返 Error 即 return / 返 nullopt 即回循环顶)收口在循环尾单一标签 `unwind_check`;用户 `throw V` 弹值 `ctx.raise(V)`(存原值不包)后同走 `unwind`。寄存器是唯一在途错误载体,try/catch 因此能同时接住 VM 检测错误与用户 throw 两类。
- `unwind()` 自最内帧向外遍历帧链(逐跳只查当前上下文的帧链,全帧未命中且在 resume 链上则跳链续搜,见下「跨 Movement 模型」),每帧以 `frame.last_ip`(指令起始指针,主循环取指前写,坑点文档 #1/#2;与 `unit->code.data()` 相减反推 offset,表保持 offset 键)查 `CodeUnit::find_try_handler`;**搜索阶段不动帧栈**(未命中的帧只记下跟踪三元组,帧引用全程有效)-> 命中 -> `Movement::unwind_to_handler(命中帧索引, record)` 一体完成:弃内层帧 + 按槽址关闭开 upvalue + 值栈截到 catch 参数槽 + `ip = handle` + 载荷 push 落槽(恰落 catch 参数槽,见坑点文档 #10)+ 清寄存器;全帧未命中且到链根 -> `Movement::reset()` 一次清场(载荷先行取走)。
- **未命中任何 handler = 本次 run 以未捕获收场,是正常结局而非 fatal**:自寄存器反提 `Error`(ObjException 经 `Error::from_baked` 保原码原消息;用户原值包 `UncaughtException`),附堆栈跟踪(§4.8),`dispatch_loop()` 返回 `std::unexpected`。此后生死归调用方:CLI 打印 message 退码 1、REPL 打印后继续下一行、嵌入方拿 Error 自行处置。`fatal_error` 只留给 Internal/Resource(解释器自身 bug/资源耗尽),与用户代码错误分轨。**`dispatch_loop` 保持返回 `Result<Value, Error>`**:客户不止 CLI(REPL/测试断言/嵌入都要 Error 而非死进程);挂起不进返回值(主上下文里 yield 即运行期错误,§4.9),两态即终态。
- **跨 Movement 模型(M6 链式多跳)**:unwind 的搜索以上下文为单位、以 resume 链为通道--本上下文全帧未命中且 `previous_` 非空时转投一跳:让位方先 `take_error()`(先于 reset,寄存器在 reset 内一并清空)再 `reset()`、`leave_coroutine(Failed)`(置 Failed 解链、恢复者置 Running 换指)、caller `raise` 载荷续搜(caller 寄存器必空:call_native 进场断言锁「进场前寄存器空」+ 切换型原生返 true 不写载荷 + 挂起期间无人可写非执行上下文的寄存器;take 到 raise 之间无 GC 点,载荷局部持有不丢根)。中间层无 handler 即连死(其 Running 瞬态被 Failed 覆盖),载荷逐跳向链根推进;链终止 = 主上下文才物化。调用点(dispatch_loop 循环尾 `unwind_check` 标签)一字不改、不收链参数,命中可能在多跳之后,调用方经循环顶自 `current_` 重取帧(坑点文档 #11 的既有纪律正好覆盖)。寄存器物理在 Movement 内(协程各自独立、互不串扰),`current_` 是 dispatch_loop/call_value 族/raise 的统一作用对象。
- **TryRecord 定稿字段**:`{begin, end, handle, stack_depth}`--不存 `frame_depth`(运行时量,编译期不可定)、不存 `catch_slot`(恒等于 `stack_depth`),见坑点文档 #5/#10。

### 4.6 GC 接入(M6,对应 gc-plan Phase 4)

> **已前拉(开发期即启用 GC)**:值栈/帧的根接线不等 M6 -- `AriaVM` 构造时即经 `gc_.set_vm_roots` 注册 tracer,collect 时标 `modules_`/`builtins_`/`registers_`/`string_constants_` 四表 + `current_` 一点(其余经对象图级联,见下);`run()` 不持 `LockGuard`,`JUMP_BACK` 是真实 safe point(`gc_.maybe_collect()`)。`Movement` 当时以纯 C++ 类落地(非 Object),tracer 直标;M6(B1)已升级 `ObjMovement : Object` 且主上下文一并 GC 分配(ctor 首笔),tracer 收敛为只标 `current_` 一点--运行链与挂起协程全部经 `ObjMovement::trace` 的 `mark_object(previous_)` 灰栈级联(`contexts_` 与链尾断言退役,§4.9)。下方描述即当前形态。

- 每个 `ObjMovement` trace 自己(对标 Wren `blackenFiber`):值栈**已用部分**(`stack_ .. top_`,顶上的垃圾不标)、每帧 `closure`(trace 级联标 function 与 upvalues)、open upvalue 链、`previous_`、挂起错误寄存器。`FrameStack::span()` 正好返回已用区间。
- GC 找到 VM 的方式:VM 向 GC 注册 mark 回调(或 GC 持不完整 `VM*` + 虚接口),避免 GC 反向依赖 VM 头文件。
- safe point:循环回边(`JUMP_BACK`)与 `new_object` 内(已落地)。`CALL` 与协程切换点不设 `maybe_collect`:切换序列零分配,首启压参的栈扩容不触 GC(Buffer allocate/reallocate 不变式),无必要。

### 4.7 原生函数(ObjNativeFn)与侧信道错误寄存器

原生函数把一个 C++ 函数包成 aria `Value`,供 builtins(`type`/`str`/`println`/...)与未来嵌入 API 使用。`CALL` 命中 `ObjNativeFn` 时**不进字节码帧**,同步直接调用。其调用约定三件套配套设计,核心是把冷路径错误踢出返回类型--错误是少发的,不该位于热路径上。

**签名**

```cpp
using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);
```

- `vm`:单一宿主句柄(对标 Lua `lua_State*` / Wren `WrenVM*` / N-API `env`)。报错 `vm.fail(code, fmt, ...)`、分配 `vm.gc()`、未来回调 aria 函数均经它;M6 协程期 `vm` 路由到当前协程,故原生函数不持、也不需要 `VMContext` 引用--单句柄即可,且自动随当前协程。
- `slots`:调用区 `[callee, a1..aN]` 的可写视图(连续 `argc+1` 个槽)。`slots[0]` = 槽 0(callee / **返回槽**),`slots[1..argc]` = 实参 a1..aN,`argc = slots.size() - 1`。
- 返回 `bool`:`true` = 成功(返回值写 `slots[0]`,见下;例外:切换型原生返 true 不写之,§4.9),`false` = 失败(已调 `vm.fail`/`vm.raise` 置寄存器,见下)。

**返回值就地写槽 0** -- 原生函数应能访问槽 0,把返回值直接写进 `slots[0]`(原地覆盖 callee)。VM 调用后 `ctx.drop(argc)` 弹掉 a1..aN,`slots[0]` 升至栈顶即返回值--比「`drop(argc+1)` + `push(result)`」省一压,且原生函数就地掌控返回槽。栈形与用户函数 `CALL` 一致(`[callee, a1..aN]`,callee 在槽 0),`peek(argc)` 即槽 0(`Movement::peek` 返回可写 `Value&`,无需新访问器)。

**方法调用形态(M5 泛化;类表读路径仅绑定 defining class 戳定的方法闭包,原生绑定形态由对象层直接构造承载)** -- 经 `ObjBoundMethod` 绑定的原生方法(内建类型方法载体):调用区 `[bound, a1..aN]` 的槽 0 为 bound 对象,VM 调用前覆写为 receiver -- 原生收到的 `slots[0]` = this,同时仍是返回槽;实参槽位与自由调用一致(实参不动槽、无整形)。类路径/静态访问(`Foo.m`)取出裸原生值不绑定,`slots[0]` = 原生自身,与自由调用无异。`init` 亦可为原生:实例化统一走 call_value 分发,Object 根类的 no-op init 即原生形态 -- 不写 `slots[0]` 即返回 this;写槽可返回任意值,嵌入 API 灵活性。

**错误走侧信道寄存器** -- 原生函数调 `vm.fail(code, fmt, ...)`(或 `vm.raise(code, detail)`)写入 `VMContext` 的挂起错误寄存器(`Movement::pending_error_`)后 `return false`;`vm.fail` 返 `FailSignal` 哨兵(按调用点返回类型隐式转换:false/nullptr/nullopt)、`vm.raise` 返 void(语句式站点),故失败路径惯用一行 `return vm.fail(...)`(同时置寄存器与返回失败),成功路径 `slots[0] = ...; return true;`。VM 在 `CALL` 后以**返回的 bool 为成败信号**--`true` 走成功路径(`entered_ctx->drop(argc)`,`slots[0]` 升至栈顶;切换后 drop 落 entered_ctx 的例外见 §4.9),`false` 后载荷留寄存器,调用方错误站点统一 `goto unwind_check` 交 `unwind` 派发/物化(§4.5)。`Error` 仅在出错时构造,不进每次调用的返回值。寄存器置于**执行上下文**而非 `AriaVM`:错误状态随上下文走,M6 协程期每个协程有独立的挂起错误(各自 raise/检查,互不串扰);M1 单一主上下文,等价于 VM 级单寄存器。`reset()` 复用上下文时一并清空;`raise` 断言当前无挂起(防嵌套 raise 未取走就再 raise)。

**bool 与寄存器的同步** -- bool 是成败信号,寄存器是错误载荷容器,二者须一致。VM 据 bool 分支:成功路径仅 debug 断言 `!has_error()` 验证契约(寄存器本就空 -- 进场已守、原生未 raise,无需 clear_error;若违约 debug 暴露,release 不静默清掉掩盖),失败路径 `take_error()` 取载荷(寄存器空则 `*` 解引用空 Opt 属 UB,debug 断言先暴露)。debug 断言 `ok == !has_error()` 捕捉两类违约:
- 调了 `vm.fail` 却 `return true`(忘 `return false`):`ok=true ∧ has_error=true` -- release 下不再 `clear_error` 掩盖,残留错误随寄存器泄漏至下次调用(违约属实现 bug,任其表面化胜于吞掉);debug 断言先暴露。
- `return false` 却没调 `raise`(声明失败无载荷):`ok=false ∧ has_error=false` -- release 下 `take_error()` 取空、`*` 解引用空 Opt 属 UB;debug 断言先暴露。
**契约:`return false` ⟺ 已调 `vm.fail`/`vm.raise`;用 `return vm.fail(...)` 即自动满足。**

**与 raise(§4.5)的关系** -- 本寄存器是 AGENTS.md「错误处理」第 2 条 `raise` 的载体:原生函数的 `vm.fail` 与 op 处理器的 `raise` 共用同一寄存器,`unwind` 查表派发逻辑见 §4.5。

**错误位置(见 §4.8)** -- 运行期位置**不烘入消息**:被抛出的错误只携带码与描述(对齐 clox/Python 惯例),位置唯一载体是未捕获出口的堆栈跟踪 `at` 行(顶帧 last_ip 恰为故障指令,原生不进帧时即 caller 的 CALL 站点);catch 侧 println(e) 不显示位置(同 Python str(e))。

**不存 arity** -- 原生函数天然变参(对标 Lua/Wren/clox),fn 自查 `slots.size()` 做元数校验,不符经 `vm.arity_error(...)` 报 `WrongArity`(措辞家族唯一口,见 `reference/error-message-style.md`)。这与 `ObjFunction.arity_`(进帧布局需要、编译期定死)的不对称由调用约定正当化:`ObjFunction` 进帧需 arity 布局部槽,`ObjNativeFn` 不进帧、无需 VM 预校验。将来若要统一可上 `ObjCallable` 基类暴露 `Opt<u8> arity()`,但当前不上(YAGNI)。

**叶子调用契约** -- 原生函数不得操作 VM 值栈(`push`/`pop`/`drop`),否则 `slots` 视图失效(值栈增长会搬迁重定位,见 §4.1)。只读 `slots[1..]`、写 `slots[0]`、经 `vm.fail`/`raise` 报错。回调 aria 函数属未来机制(由 `vm` 提供,自管栈纪律;接缝已备:`AriaVM::run_closure(closure)`--私有执行本体,压 callee + 经 `call_closure` 进帧 + 驱动 `dispatch_loop`,无入口装饰、不 reset/不播源根/不断言主上下文;callable 收敛为闭包(M4)后接缝通货即闭包,裸 `ObjFunction` 的现场包装归入口仪式 `run()`,落地重入时升公开并补 `dispatch_loop` 按基线帧深退出(现仅 `frames().empty()` 返回,中途重入会穿掉调用者帧)与实参布线)。GC 已启用(值栈/帧接根),原生函数内可经 `vm.gc()` 分配(`new_string`/`new_object` 等);跨分配持有的中间对象须 `Guard` 入临时根,`slots[0]` 写入后即随值栈为根。

**内建作者体感**(从 `Result<Value, Error>` 的啰嗦降到一行):

```cpp
bool str_native(AriaVM& vm, Span<Value> slots) {
    const auto argc = slots.size() - 1;
    if (argc != 1) { return vm.arity_error(argc, "str", 1); } // 元数报错唯一口(措辞家族见 error-message-style.md)
    slots[0] = Value::from_obj(new_string(vm.gc(), format_value(slots[1])));  // 就地返回
    return true;
}
```

**落地**:类型 `src/object/ObjNativeFn.{hpp,cpp}`;寄存器 `Movement::pending_error_` + `raise/has_error/take_error/clear_error`;`AriaVM::raise`(void)转发到当前上下文、`fail` 返 `FailSignal` 哨兵(false/nullptr/nullopt 按调用点转换);`call_value` 加 `ObjNativeFn` 分支(bool 成败信号 + 槽 0 返回 + 寄存器载荷)。`tests/runtime/test_ariavm.cpp` 4 例(槽 0 返回 / 零元 / 侧信道错误 / 元数自查)。内置函数注册机制见 §7。

### 4.8 运行时位置标注与未捕获堆栈跟踪

**行级粒度** -- 字节码只有 RLE 行号表(`CodeUnit::line_for_offset`,二分查行),无列号 -> 运行期位置上限为行(对标 Lua);列号须扩行号表,暂不做。消息形态:运行期为 `"Category: Name detail"`(**不含位置前缀**;编译期为 `path:line:col: Category: Name detail` -- 编译错误无堆栈,位置是唯一锚点故保留)。运行期位置仅出现在未捕获跟踪行,位置串由 `ObjModule::format_location(line)` 渲染(文件模块 = `abs_path():line`;合成模块(名以 `<` 开头,如 `<script>`)与无目录锚点无文件身份 -- `abs_path()` 会拼出伪路径 -- 退化为 `"<模块名>:<line>"`)。

**位置由未捕获跟踪行统一给出** -- 三类报错站点(VM 检测错误如算术/LOAD_GLOBAL、`call_*` 失败的 WrongArity/StackOverflow/CallNonCallable、原生函数 `vm.fail`)的细节串皆无位置,装箱也不烘位置:消息 = `Error::make_message(code, detail)`(无位置版)。错误位置的取得依赖 unwind 帧遍历:未捕获时逐帧收集 `(function, module, last_ip)` 三元组,顶帧 = 故障帧、`call_*` 失败即 caller 帧(被调帧未进)、原生报错即 caller 帧(原生不进帧),其 `last_ip`(主循环取指前写)恰为故障指令/CALL 站点,无需 take 点补标、无双重标注。(码, 烘焙消息) 拼好跟踪后 `from_baked` 一次物化,同源同串。帧栈空(run 外直调)无位置。位置串是 C++ 侧 String 拼接,不添 GC 约束(`new_exception` 自守不变,坑点文档 #8)。

**透传错误不标注、无跟踪** -- 被导入模块的编译期 Error 位置已烘为**被导入文件**的 `path:line:col:`,经 IMPORT 原样透传(现有语义),二次标注会得双重位置且类别语义混乱。实现上与「运行期错误当场构造」分路:后者经 raise 装箱(无位置),前者直传原 Error(不经 unwind,亦无堆栈跟踪)。

**未捕获堆栈跟踪:unwind 时逐帧收集、物化时烘焙,不存 ObjException** -- 跟踪在 uncaught 出口一次性生成(对标 Python:traceback 取自活帧,异常对象不背全程 trace;catch 掉的异常大多数用不上,逐次 throw 收集不值得)。落点:`unwind()` 遍历帧链搜索 handler 时,对**未命中的帧**顺带收集 `(function, module, last_ip)` 三元组(搜索阶段不动帧栈、帧引用全程有效;若等到「所有帧无 handler」再去查,帧已全弹、无帧可查,故须顺路收集)。`last_ip` 在此**两用**:unwind 查表(坑 #1/#2)、跟踪行号--顶帧 = 故障指令、外层帧 = CALL 站点,恰是「该帧执行到哪」的正确答案。走到未捕获出口物化 Error 时,按收集序(内->外,最内帧紧贴错误消息行,主流 traceback 惯例)格式化为逐帧 `  at <fn名> (<位置串>)` 行附加到 `Error::message_` 尾部;命中 handler 则收集弃用。**每跳重新收集(M6,跟踪截断在协程边界)**:trace 是 unwind 跳链循环的逐跳局部,死在边界的协程帧不并入物化侧的跟踪--物化只含最终命中/物化那一层的帧链,最内层 at 行 = resume 调用点(原生不进帧,与上文「原生报错即 caller 帧」同源)。烘焙进 message_ 而非 VM 直接输出:与「message 一次性烘焙、Error 自足」一致,`interpret_run` 打印零改动、测试可断言、嵌入方自行决定展示。

### 4.9 协程:单循环切换模型(M6 已落地,Wren 对照)

> 协程切换采用单循环显式切换模型,对标 Wren 0.4 的 fiber:整个 VM 任一时刻只有一个 `dispatch_loop()` 循环在执行,切换发生在循环知晓的唯一位置(CALL 善后点),不嵌套解释器调用、不依赖任何「切换纪律」约定。重入式方案(协程深处 `yield` 需嵌套 `dispatch_loop()` 或把 yield 做成操作码层层退栈)被此模型整体免除。

**机制四件套**(Wren 源码索引见 §8):

1. **create/resume/yield/status 是原生函数,不是操作码**(对标 Wren `Fiber.call/transfer/try`(共用 `runFiber`)与 `Fiber.yield`:全部原语,无 YIELD 操作码)。四原语住友元宿主类 `CoroutineModule`(AriaVM 的私有静态方法 + 方法表全收私有,唯一公有口 `register_functions`;AriaVM 友元以访问 `check_arity`/`prepare_call_args`/`current_` 等私有面);语言面载体是 ctor 临界区 `bootstrap_coroutine_module` 建的合成 `<coroutine>` 模块(方法表装进其 globals,模块经 `builtins_["coroutine"]` 可达、永不入 `modules_`)。resume 职责:校验(元数 → 类型 → `state_` 五态穷举 switch:Suspended 唯一可恢复,Done/Failed 报死态、Normal/Running 报在链上)→ 完成对**对侧**上下文的准备与写值(契约见下方「切换协议」;调用者侧栈一律留给 call_native 统一收敛)→ 经切换原语对换指 → 返回。
2. **切换对 `call_native` 透明,CALL case 零改动**:`call_native` 以 `entered_ctx`(调用发生时的上下文,事后簿记的真实锚点)为统一锚点--成功路径的 `drop(argc)` 与寄存器断言一律落在其上:未切换时它即 `current_`,已切换时它恰为刚被挂起的旧上下文,`drop` 恰好完成调用者侧栈归一,**无需分支、无需探测比较**(协议见下)。CALL case 更无需感知:成功路径本就只剩 `break`,循环顶自新 `current_` 取指。**aria 的切换同步成本为零**:解释器状态全堆驻留(`ip` 在 `CallFrame`、栈顶在 `Movement::top_`、每指令重取帧引用),没有可陈旧的循环级局部副本--相对 Wren 的简化(Wren 因 frame/ip/stackTop 缓存于 C 局部寄存器变量,切换前后须 `STORE_FRAME`/`LOAD_FRAME` 一对宏同步,aria 无此负担)。
3. **RETURN 完成路径**:协程最外帧弹尽且有 `previous_` → 完成值写恢复者预留槽(`resumer->peek(0)`)、`reset()`(死协程不留栈,先关开指)、`leave_coroutine(Done)` 置 Done 解链换指(对标 Wren RETURN:numFrames==0 且有 caller 则切回并写返回值);无 `previous_`(主上下文顶层)→ `dispatch_loop()` 返回主模块对象(入口收尾 ModuleEntry 所压)。HALT case 另断言协程内不可达(手写字节码逃生口的防线)。
4. **`run()` 两态与链根锚**:主上下文里 yield 即运行期错误(`YieldOutsideCoroutine`)--挂起只发生在协程内,run() 出口恒顶层 RETURN / 未捕获物化、必发生在 resume 链链根。resume/yield 的深处切换不产生 run 返回,`dispatch_loop()` 因此**永不重入**;`run()` 入口锚 `entry_ctx = current_` + 出口断言 `current_ == entry_ctx` 为永久不变式(run() 是唯一驱动入口,漏交接当场炸)。

**切换协议(落地契约)--对 `call_native` 透明,无需探测**:

```cpp
// call_native(现行实现,完整形状):entered_ctx 是全部事后簿记(drop/寄存器断言)的锚点。
const auto entered_ctx = current_;
const auto slots       = Span<Value>{&current_->peek(argc), static_cast<usize>(argc + 1)};
ASSERT(!current_->has_error(), "pending error not cleared before native call");
if (obj->fn()(*this, slots)) {
    // 成功契约对调用方寄存器断言:entered_ctx 未切换时 == current_,已切换时是被挂起的
    // 旧上下文 -- 两侧都是「调用发生时的上下文」,新上下文的寄存器不归本次调用管。
    ASSERT(!entered_ctx->has_error(), "native fn returned true but raised error");
    // 调用者侧 CALL 栈效应统一落在 entered_ctx 上,无分支:未切换即正常路径的 drop(argc)
    // (弹实参,槽 0 升顶即返回值);已切换时恰为对旧上下文做同样的 drop -- 弹实参、留
    // 槽 0 为预留结果槽(由对侧原语写 yield 值/完成值)。
    entered_ctx->drop(argc);
    return true;                        // CALL case 只剩 break,循环顶自新 current_ 取指
}
// false ⟺ 已 raise 的二值契约不变:切换型原生函数出错时先 fail 进调用者、**不切换**;
// 「禁止 false + 切换」为永久契约(断言钉)。
ASSERT(current_ == entered_ctx, "native fn returned false after switching current_");
ASSERT(entered_ctx->has_error(), "native fn returned false but raised no error");
return false;                           // 载荷留寄存器,调用方 goto unwind_check
```

切换型原生函数(`coroutine.resume`/`co.yield`)三条契约:

1. **成败分流,只碰对侧上下文**:切换成功 → 首启压载荷 + `prepare_call_args` 整形 + `enter_frame` 进帧(协程栈无预留槽的唯一不对称处)/ 已挂起把载荷写进对侧预留槽(`co->peek(0)`,即 yield 表达式的值),换指经切换原语对 `enter_coroutine`(resume 侧)与 `leave_coroutine(departing_state)`(yield 置 Suspended、完成置 Done、未捕获跳链置 Failed 三方向共用);任何校验失败 → **不切换**,`vm.fail` 进调用者寄存器后返回 false。错误通道与切换通道彻底分流,bool 契约保持二值,非切换型原生函数零影响。**调用者侧的栈不动**--`[callee, a1..aN]` 原样留给 call_native 统一 drop;这同时是 GC 红利:实参(含协程对象自身)在原生函数执行全程都躺在调用者值栈上为根。
2. **调用者侧栈效应由 call_native 统一在 `entered_ctx` 上收敛(时点:切换后、切回前),不得推迟到切回方**:已切换时 `entered_ctx->drop(argc)` 弹实参、留槽 0 为预留结果槽;yield 值/完成值/resume 载荷由对侧原语直接写对侧预留槽(`peek(0) = 值`)。不能留给切回方(yield/RETURN)清理--**切回方对本上下文当年那次 CALL 的 argc 一无所知**,且残槽会跨挂起期滞留值栈;归一后跨挂起期两侧栈形状恒定,任何一侧都不依赖另一侧的局部信息(Wren 的对应做法是切换型原语在 `runFiber`/`fiber_yield` 里两侧手工挪 `stackTop`,aria 把调用者侧收进 call_native 统一执行,切换型原生函数的契约面更小)。对侧死于未捕获错误时载荷走恢复者寄存器(链式 unwind,§4.5),该槽随调用区被 unwind_to_handler 截栈一并丢弃、无人补写。
3. **顺序不变式(住 `enter_coroutine` 注释)**:置链先于换指--换指后 resumer 只经 `co->previous_` 这一条边可达(GC 根只标 `current_`),首启切换后 `prepare_call_args` 的垫充/varargs 打包经 `new_object` 顶 `maybe_collect`,恢复者与其实参以此边存活(实参留栈到 drop 是契约 1 的红利)。

**`previous_` 对齐 Wren caller 语义**:resume 时设(`enter_coroutine`)、**yield、完成与未捕获跳链都解链**(`leave_coroutine`)。`previous_` 的含义是「下一次让位时回到哪」,不是持久 resume 链;挂起态恒空是安全前提(resume 的已挂起臂、yield 判主上下文都读它)。置链/解链与状态写点成对(切换点唯一收口在原语对),故「`state_` 五态穷举」与「`previous_` 判链」在用户可达协程上严格等价--aria 取状态单源判定,覆盖显式(对标 Wren:caller 在 yield 与 RETURN 完成时都置 NULL,"Fiber has already been called" 仅在 caller 非空时报)。主上下文 uniform 参与换位(resume 置 Normal、让位置 Running)但其 `state_` 语言不可达(status 无路径),不参与任何判定。

**GC**:主上下文一并 GC 分配(ctor 首笔 `gc_.new_object<ObjMovement>`),VM 级协程根仅 `current_` 一个,tracer 只标这一点--运行链(`current_ -> previous_ -> ...` 的中间层)与挂起协程(用户持有的协程值)全部经 `ObjMovement::trace` 的 `mark_object(previous_)` 灰栈级联;`trace` 对标 Wren `blackenFiber`:值栈已用区间 + 各帧 `closure`/`module`(closure 级联标 function 与 upvalues)+ open upvalue 链 + `previous_` + 挂起错误寄存器。**退役的是 `contexts_` 调度列表、链尾断言与独立的 `main_ctx_` 成员**--切换只发生在结构性位置,没有可违反的纪律;链根交接由 run() 入口锚/出口断言钉(§3)。

**错误跨协程**(接 §4.5):协程 A 内未捕获(unwind 遍历 A 的帧链无 handler)→ A `take_error()` 取载荷、`reset()` 清场、置 Failed 解链,把载荷 raise 进 resume 调用者的挂起寄存器,unwind **就地**继续搜调用者的帧链--中间层无 handler 即连死,载荷逐跳上投至链根(主上下文)才物化。设计初稿曾设想「等价于 resume 作为一次失败的原生调用,调用方按既有 bool 契约取出」--该说法按字面不成立:resume 切走时 `entered_ctx->drop(argc)` 已执行、`return true` 已返回,CALL 善后点早已收工,主循环没有逐指令的 `has_error()` 检查,恢复者侧没有任何时机消费该载荷;正确形态就是 unwind 自身跳链(调用点 `unwind_check` 一字不改)。对标 Wren `runtimeError` 沿 caller 链逐 fiber 中止、遇 `FIBER_TRY` 调用者则错误值写入其调用槽并恢复之;aria 的寄存器模型无需 Wren 的 `fiber->error` 专用字段。跟踪截断(每跳重收集,at 行只含接收侧帧链)见 §4.8。

**M6 落地记录**(2026-09,B1-B3):

1. B1(96aa55e/bc93d79/d0ac244/12bee08):`ObjMovement : Object` + `ObjType::MOVEMENT` + trace 收口;主上下文堆化(ctor 首笔 GC 分配,`main_ctx_` 成员退役),tracer 收敛为只标 `current_` 一点,`contexts_`/链尾断言退役;run() 入口锚 + 出口断言钉链根交接。
2. B2(8adae5c/5b3151a/e1508fc):`ExecState` 五态 + 状态拼写常量、`new_movement` 工厂;四原语(友元宿主类 `CoroutineModule` + 合成 `<coroutine>` 模块 + `Builtin::register_module_functions` 底座)、`bootstrap_coroutine_module`、RETURN 完成切回、HALT 断言、`call_native` 删成功路径守卫(false 路径「禁止 false+切换」保留为永久契约)。
3. B3(310a20c):跨协程错误--`unwind()` 链式多跳(本节「错误跨协程」)+ 跟踪截断(§4.8)。

对初稿的两处偏离:`ExecOutcome` 三态未采用(主上下文里 yield 直接报运行期错误,run() 维持两态);`main_ctx_` 不以值成员保留而整体堆化(换来 tracer 单点与主/协程类型统一)。坑点与不变式归档见 `coroutine-implementation-pitfalls.md`。

**性能注记**:Wren 以 C 局部寄存器缓存 frame/ip/stackStart 换取每指令速度,代价是切换须显式同步;aria 目前反其道(全堆驻留,切换零成本,每指令多几次内存载入)。若将来测得热路径受损,可引入「指令内工作副本 + 指令边界写回」的缓存--注意快照的正确形态是**指令级 + 显式同步点**,而非「整个 `dispatch_loop` 生命期的入口快照」(后者已废弃,见 runtime.md)。

### 4.10 派发策略:单 switch 跳表(computed goto 实测后弃用)

`dispatch_loop` 保持单一 switch,由编译器 Lower 成单点跳表派发。曾落地过 computed goto 双形态(`TARGET`/`DISPATCH` 宏 + 标签地址跳表,CMake 开关 `ARIA_USE_COMPUTED_GOTO`,机制见 commit 264acab,本批已删),并在 Apple Silicon + clang 上做了两基准 A/B 与二进制取证,结论:

- **clang 系优化器会把全部派发尾尾合并成共享 trampoline,per-site 间接分支在二进制里根本不存在**;而 indirectbr 的 CFG 语义又迫使优化器在共享派发点做「全状态广播」(把常驻状态复制进大量寄存器与栈槽),代码体量与每派发指令数双增,实测显著更慢。逐站点唯一指令可强制保住 per-site 分支,但真 per-site 也不赢--现代宽核的分支预测对单一跳表已无余利可挖。
- **GCC 系能兑现 per-site 且无广播成本,派发热路径确有小胜**;但其 switch 被编成比较链、基线本身弱,语言级(整程序)收益微小且在浮点重型负载上大幅回退,绝对水平仍全面落后 clang 的 switch 构建,不足以养双形态基础设施。
- CPython/Lua 同为「switch 与 computed goto 两形态按编译器条件择一」,其保留 computed goto 的前提是主场 GCC 上有两位数收益;aria 实测不存在这个前提。

结论:**默认恒为 switch 形态**;若未来主平台/主编译器变化且实测 computed goto 净赢,从 264acab 复活机制再评估。测量原始数据与取证细节见仓库根 `computed-goto-bench-ab.md`(测量工件,不入库)。

## 5. 早期简化(M1 的刻意收敛,均已演化)

M1 目标只有一句话:**让一个手写/编译产出的 CodeUnit 在 VM 里跑完,值栈与帧栈行为正确**。当时的收敛项现状:

- **不继承 Object**:值栈/帧/open upvalue 开链已经 vm_roots tracer 接 GC 根(见 §4.6「已前拉」),`run()` 不禁 GC,`JUMP_BACK` 已是 safe point;M6 已升级 `ObjMovement : Object` 入对象链表(trace 收口到对象自身、主上下文一并堆化),协程根收敛 `current_` 单点(§4.9,不设 movements_ 并集)。
- **闭包已闭环(M4)**:`CallFrame` 持 `ObjClosure*`(callable 收敛为闭包,顶层入口也是闭包),`CLOSURE`/`LOAD_UPVALUE`/`STORE_UPVALUE`/`CLOSE_UPVALUE` 四指令实装,open upvalue 开链 + 值栈增长第三类重绑已落地(见 §4.1);M5 类指令(九 opcode)已实装、编译发射已翻转落地。
- **异常已闭环(M3)**:挂起错误寄存器升为运行期主通道,`unwind` 查表派发(见 §4.5 与坑点文档)。
- `dispatch_loop()` 永不重入(M6 单循环切换模型,§4.9);循环状态全部堆驻留于 Movement/CallFrame(无 C 局部工作副本,每指令自 `current_` 重取),这一性质即 M6「切换零同步成本」的来源。

## 6. 实施路线

| 阶段 | 内容 | 验收 |
| :--- | :--- | :--- |
| **M1 跑起来(已落地)** | `Movement`(纯 C++ 类)+ `VMContext` 别名;`CallFrame`(持 `ObjFunction*`);`AriaVM::run()`:`LOAD_CONST/LOAD_IMM/LOAD_NIL/TRUE/FALSE`、局部槽(含 `_L`)、算术/比较/`NOT`/`NEGATE`、`POP/POP_N/DUP/DUP2`、`JUMP*`/`JUMP_BACK`、`CALL`(`ObjFunction` 进帧 + `ObjNativeFn` 同步调用,见 §4.7)、`RETURN`、`HALT`。值栈可增长;`VMContext` 挂起错误寄存器(§4.7,M1 `raise` 切片) | 手写字节码算术/循环/函数调用/原生函数跑通 |
| **M2 全局与模块(已落地)** | `ObjModule`、模块表、`DEF/LOAD/STORE_GLOBAL`、内置函数注册机制(指令集 §6.4 待决项在此定) | 模块顶层 var/fun 可定义可读 |
| **M3 异常(已落地)** | `TryRecord` 定稿字段、统一寄存器传播 + `unwind`、`THROW`、运行期位置标注与未捕获堆栈跟踪(§4.8);finally 不做(裁撤,善后后继 defer 为可选后续,见 grammar.txt 说明区与坑点文档裁撤记录) | try/catch 单测,跨帧 unwind 正确 |
| **M4 闭包(已落地)** | `ObjClosure`/`ObjUpvalue`、`CLOSURE`、open upvalue 开链(按槽址降序)、`CallFrame` 换持 `ObjClosure*`(callable 收敛为闭包,顶层入口也是闭包,`ObjFunction` 退为常量池内部物)、值栈增长第三类重绑(§4.1)、编译翻转(`resolve_upvalue` 递归捕获解析 + `CLOSE_UPVALUE` 作用域退出批量关闭)。语义模型「捕获即引用」(Lua/clox 式);defer 善后为可选后续 | 计数器闭包等经典样例正确,NaN-boxing 与 TagValue 双值表示配置下全绿 |
| **M5 类与对象(已落地)** | `ObjClass`/`ObjInstance`/`ObjBoundMethod`、`MAKE_*` 系列、bootstrap Object 根类、`init` 缓存(指令集 §5.5;六项设计决策:无 meta/静态+方法单表/构造期 bootstrap Object/bound 缓存进实例 fields 表(三铁则；已反转取消,见 `.claude/rules/object.md`「类成员读写与绑定方法不缓存」)/STORE_FIELD 与 MAKE_STATIC 镜像双指令/defining class 挂 ObjClosure) | 类定义/实例化/继承/super 样例通过 |
| **M6 协程 + GC 根(已落地)** | `ObjMovement : Object`(重命名 + trace + `ObjType::MOVEMENT`,主上下文一并 GC 分配)、GC 根收敛 `current_` 单点、**单循环切换模型**(§4.9):`coroutine.create/resume/yield/status` 原生函数(友元宿主类 + 合成 `<coroutine>` 模块)+ call_native 善后落 `entered_ctx` + RETURN 完成切回解链 + 跨协程错误 `unwind` 链式多跳;主上下文里 yield 即运行期错误(`run()` 维持两态,`ExecOutcome` 退役) | 协程生成器样例;stress GC 下多协程无悬垂;三上下文连死多跳与跟踪截断语料 |

顺序依赖:M4 依赖 M1 的帧/栈;M5 依赖 M4(方法即闭包);M6 依赖全部。M2/M3 可与 M4 并行。字节码编译器(AST->CodeUnit)已落地(CodeGen,43 个 visit)。

### M1 验证状态

- **局部槽填充约定**(见指令集文档 §4.3):帧的 `slots` 指向槽 0(callee),槽区不自动保留,故编译器**声明时不预占**--声明名登记时初始化器值(或 `LOAD_NIL`)恰好压在该槽位完成填充(「下个局部 slot = 当前栈高」,无 store/pop)。若预占发 `LOAD_NIL` 再覆写,首个临时值就会与预占槽错位。
- **短路跳转的 L_end 是 `<b>` 之后的汇合点**(非 `<b>` 之前);跳转偏移以读完操作数后的 ip 为基准(与 Disassembler 解码一致),手写回填需按此计算。
- 除法/真值语义(已定,指令集 §9 #8):int/int 截断整除、除/模零报运行时错误、f64 按 IEEE(除零得 inf/nan)、真值为 Lua 风格(仅 nil/false 为假)。

## 7. 缺口清单(实施前需补的东西)

- **协程不新增指令**(M6 定稿并落地,§4.9):create/resume/yield/status 走原生函数通道(合成 `<coroutine>` 模块,经 `builtins_["coroutine"]` 可达),指令集零增补。
- `ObjType::MOVEMENT` 已增(语言可见名 `Movement`,`type(co)` 报之)。
- **`TryRecord` 字段已定稿**:`{begin, end, handle, stack_depth}`(无 `frame_depth`/`catch_slot`,见坑点文档 #5/#10),`find_try_handler` 返 `Opt<const TryRecord*>`。
- **GC 的 VM 根回调接口**已落地(`GC::set_vm_roots`,AriaVM 构造期注册)。
- **内置函数注册机制**(方案 B「VM 级 builtins 表 + LOAD_GLOBAL 回退」,无新指令):`src/runtime/builtins/Builtins.{hpp,cpp}` 的 `Builtin::register_builtin(GC&, AriaHashTable&)` 把 `type`/`str`/`println`/`assert` 等经 `new_native_fn` 包成 `ObjNativeFn` 后按名 `set` 进 AriaVM 的 `builtins_` 表(VM 级 `AriaHashTable`,全 VM 共享一份;`print` 关键字与 `PRINT` 指令已整体移除,输出口是 `println` 内建;另经 `bootstrap_coroutine_module` 挂 `coroutine` 键 -> 合成 `<coroutine>` 模块)。注入点唯一:AriaVM 构造期 `set_vm_roots` 之后调用一次。`LOAD_GLOBAL` 先查当前模块 globals,miss 回退 `builtins_`(Python 式 globals -> builtins 查找链);`STORE_GLOBAL` **不**回退 builtins(赋值不隐式创建,必须先 var 声明,见 `docs/grammar.txt`「作用域模型」的裸名赋值条),仅 `DEF_GLOBAL` 写模块 globals 可 shadow 内置。intern 池保证 CodeGen 发射 `LOAD_GLOBAL "name"` 与注册名同指。不取方案 A「按模块预填 globals」(会在 REPL 逐行 `run()` 重注册、覆写用户 shadow);方案 B 一份只读表回避之,并省每模块 4 个 `ObjNativeFn` 分配。原生函数类型与 CALL 路径见 §4.7。
- **VM 与 GC 的拥有关系**:已定 -- VM 拥有 `GC gc_` 值成员(每 VM 一个 GC,REPL 常驻)。

## 8. 参考

- `.claude/reference/bytecode/bytecode-instruction-set.md`:指令集/操作数编码/栈效应/lowering(VM 实现的操作语义基准)。
- `src/runtime/FrameStack.hpp`:帧栈模板 + `truncate`(unwind 用)。
- `src/bytecode/CodeUnit.hpp`:`TryRecord`/`find_try_handler`(异常查表已就绪)。
- AGENTS.md「错误处理」第 2 条:VM 自管异常的设计目标。
- `.claude/reference/runtime/exception-implementation-pitfalls.md`:M3 异常实现踩坑归档(本文 §4.5/§4.8 定稿的实现级细节与坑编号 #1-#16;M4 补录闭包 upvalue 关闭与 unwind 截栈/弹帧交互的坑点)。
- `.claude/reference/runtime/coroutine-implementation-pitfalls.md`:M6 协程实现踩坑归档(§4.9 的实现级细节:槽协议/切换根安全/链式 unwind 定序/嵌套 run_closure 红线)。
- **Wren 0.4 源码**(§4.9 单循环切换模型的参考实现;本地副本 `/Users/icelake/src/wren`,上游 wren.io/wren):`runInterpreter`(`wren_vm.c`,循环缓存 + `STORE_FRAME`/`LOAD_FRAME` 同步、CALL 原语善后「采用被换走的 fiber」、RETURN 完成切回、`RUNTIME_ERROR` 宏)、`runtimeError`(错误沿 caller 链传播)、`runFiber`/`fiber_yield`/`fiber_suspend`(`wren_core.c`,fiber 原语族与返回槽契约)、`blackenFiber`(`wren_value.c`,协程 GC 标记)。
