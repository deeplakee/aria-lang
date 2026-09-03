# VM 设计与实施计划

本文档规划 `AriaVM` 与执行上下文的设计,并给出分阶段实施路线。基准:`bytecode-instruction-set.md`(指令集/栈效应/操作数编码)、`gc-implementation-plan.md` Phase 4(协程单元 + VM 根)、`FrameStack.hpp`、`CodeUnit.hpp`(含 `TryRecord`/`find_try_handler`)。

> **实施原则:先让虚拟机跑起来。** 完整的协程/GC 根/闭包等机制按阶段推进(见 §6),早期阶段(M1)刻意收敛到最小,不在第一步背上全部设计复杂度。

## 1. 命名决定

- 协程/执行上下文实体类名:**`ObjMovement`**(最终形态,`Object` 子类型,与 `gc-implementation-plan.md` Phase 4 的 "Movement" 一脉相承)。
- **`VMContext` 是 `ObjMovement` 的别名**:`using VMContext = ObjMovement;`。代码中按语义选用--泛指「一段执行的状态」时用 `VMContext`,强调「协程对象」时用 `ObjMovement`。
- `ObjType` 需增补 `MOVEMENT` 枚举值(当前枚举没有,升级为 Object 时一并加,见 §6 M6)。
- 早期(M1)该类先以纯 C++ 类 `Movement` 落地(`using VMContext = Movement;`),不继承 Object;M6 升级为 `ObjMovement : Object` 时仅重命名 + 加 trace,别名的存在使调用方代码零改动。

## 2. 状态分层

VM 的全部状态按「谁拥有」切两层,`VMContext` 的引入点即在此:

| 层 | 状态 | 特征 |
| :--- | :--- | :--- |
| **解释器级(AriaVM,共享)** | GC、模块表(每模块自己的 globals)、内置函数注册、Object 根类 bootstrap、`current_`(正在运行的上下文)、`contexts_`(所有活上下文 = GC 根集合) | 整个进程一份,所有协程共享 |
| **执行级(VMContext,每协程一份)** | 值栈、`FrameStack<CallFrame> frames_`、open upvalue 链头、执行状态机、挂起错误寄存器(侧信道,见 §4.7)、(协程期)resume 传入值等 | 协程的实体就是这堆状态 |

核心结论:**主上下文与协程同构**--主上下文就是「永不 yield 的协程」,`run()` 循环对二者零特判。协程要是一等值(可存入 list、可传参),故最终形态必须是 `Object` 子类型;主上下文作为 Object 多付的只是对象头(几十字节),相对值栈(约 KB)可忽略。

## 3. 结构草图

```cpp
// ---- 执行级 ----
class ObjMovement /* final : public Object, M6 起继承 */ {
    // 值栈:初始定容、push 溢出时 2x 增长(搬迁时重定位 top_ 与活动帧 slots,见 §4.1)
    Value*        stack_;       // GC 分配,初始 kStackInit 个 Value,可增长
    Value*        top_;         // 栈顶指针,热路径裸指针操作
    FrameStack<CallFrame, kFrameMax> frames_;
    ObjUpvalue*   open_upvalues_ = nullptr; // 开指 upvalue 链(M4 起用)
    ExecState     state_ = ExecState::Suspended; // Suspended/Running/Done/Failed
    // M6 增:resume 传入值、caller 链等协程期字段
};

// 帧是纯 POD(FrameStack 要求 trivially-copyable;只持指针,满足约束)
struct CallFrame {
    ObjClosure* closure_;   // 顶层也是闭包;M1-M3 过渡期持 ObjFunction*
    CodeUnit*   unit_;      // 缓存 closure_->fn()->unit(),省每条指令一跳
    u8*         ip_;        // 裸指针最快;raise 等冷路径按需算 offset
    Value*      slots_;     // 本帧局部基址(callee=槽0、this/参数、局部)
};

// ---- 解释器级 ----
class AriaVM {
    GC               gc_;          // VM 拥有 GC 值成员(已定:每 VM 一个 GC)
    AriaHashTable    modules_;     // 模块表(键=规范路径 ObjString*、值=ObjModule*,均装箱 Value)
    Movement         main_ctx_;    // 主上下文(值栈 + 帧栈;M6 协程期升级 ObjMovement : Object)
    List<String>     source_roots_;// 源根列表([0]=入口根、[1..]=配置根)
    // Movement*      current_;     // M6:多协程时当前上下文(现单上下文,直接用 main_ctx_)
    // List<Movement*> contexts_;   // M6:所有活上下文 = GC 根集合

    ExecOutcome run();             // 驱动 current_ 直到 返回/yield/未捕获异常
    void        raise(Error&&);    // 查 TryRecord 表 -> truncate -> 跳 handler(M3 起用)
    Result<Value, Error> call_value(Value callee, u8 argc); // CALL 与嵌入 API 共用
};

// run() 的结果:为什么停下来。协程挂起的表达即在此
struct ExecOutcome {
    enum class Kind { Returned, Yielded, Uncaught } kind_;
    Value value_;   // 返回值 / yield 值 / 异常值
};
// M1 实现简化:run() 返两态 Result<Value, Error>(无 Yielded);M6 引入 yield 后扩为本三态 ExecOutcome。
```

`resume` = 把目标上下文设为 `current_`,再次进入 `run()`;`yield` = 当前帧存好 `ip_`,`run()` 返回 `Yielded`。同一个 `run()` 循环服务主上下文与协程。

## 4. 关键决策

### 4.1 值栈可增长,搬迁时重定位指针

值栈初始定容(`kStackInit`)、`push` 溢出时 2x 增长:整体搬迁(GC reallocate,内部 memcpy)后,把 `top_` 与所有活动帧的 `slots` 按新旧基址差重定位。增长打破「指针绝对稳定」,故每次增长后指进值栈的裸指针都必须重定位——当前只有 `top_` 与 `CallFrame::slots` 两类;`ObjUpvalue` 开指落地(M4/M6)后,须在此一并修其持的 `Value*`(或改索引式 upvalue,免逐条修)。帧栈 `FrameStack` 仍一次分配永不扩容:帧数少、无需增长,且其指针稳定性不受值栈搬迁影响。

### 4.2 值栈不复用 `FrameStack<Value, N>`

帧栈是「acquire/pop/truncate」的槽位语义;值栈热路径需要 `push/peek(k)/Value* 寻址`(callee 的参数就躺在栈顶,直接作为其 slots 区)。语义不重合,`Movement` 自持 `UPtr<Value[]> + top_` 裸指针管理。容量参考:值栈初始 `kStackInit` = 1024 个 Value(8KB)、`push` 溢出 2x 增长;`kFrameMax` = 256(帧栈仍定容)。每上下文初始内存约 8KB 量级,数千协程无压力。

### 4.3 `ip` 用裸指针,offset 按需换算

热路径 `*ip_++` 取操作数;`find_try_handler(u32)`/行号表吃 offset,但都在冷路径(raise/报错),`offset = ip - unit->code.data()` 一次减法。

### 4.4 帧引用可缓存于循环外

`FrameStack` 永不扩容 => `CallFrame&` 引用整轮循环稳定,可像 clox 一样把 `frame`/`slots` 缓存进局部,CALL/RETURN 时刷新。

### 4.5 异常衔接(与 CLAUDE.md「错误处理」第 2 条一致)

- op 处理局部失败 -> `Result<Value, Error>` 返回;
- `run()` 收到失败 -> `raise(Error)`:**只查当前上下文的帧链**(协程异常不跨协程传播,各自 unwind);
- `CodeUnit::find_try_handler(offset)`(已实现)命中 -> `frames_.truncate(frame_depth)` + 值栈截断(`top_` 复位)+ 压异常值 + `ip = handler`;
- 未命中任何 handler -> 上下文终止(`Failed`),返回 `Uncaught`;主上下文 Failed 才终止程序。
- **TryRecord 需扩字段**:当前 `{begin, end, handle}` 缺 `stack_depth`/`frame_depth`/`catch_slot`(M3 随编译器 try 支持一并补,`find_try_handler` 逻辑不变)。

### 4.6 GC 接入(M6,对应 gc-plan Phase 4)

> **已前拉(开发期即启用 GC)**:值栈/帧的根接线不再等 M6 -- 当前 `AriaVM` 构造时即经 `gc_.set_vm_roots` 注册 tracer,collect 时标 `main_ctx_` 值栈 `[base, top)` + 各活动帧 `function`/`module` + `modules_`;`run()` 不再持 `LockGuard`,`JUMP_BACK` 已是真实 safe point(`gc_.maybe_collect()`)。`Movement` 仍是纯 C++ 类(非 Object),以 tracer 直标代替升 Object;M6 升级 `ObjMovement : Object` 入对象链表 + 接 open upvalue 链 + 多协程 `contexts_` 并集标根。下方描述为 M6 目标形态。

- `contexts_` 中每个上下文 trace 自己:值栈**已用部分**(`stack_ .. top_`,顶上的垃圾不标)、每帧 `closure_`、open upvalue 链。`FrameStack::span()` 正好返回已用区间。
- GC 找到 VM 的方式:VM 向 GC 注册 mark 回调(或 GC 持不完整 `VM*` + 虚接口),避免 GC 反向依赖 VM 头文件。
- safe point:`CALL`、循环回边(`JUMP_BACK`)、`new_object` 内、协程切换点。当前已落地 `JUMP_BACK` + `new_object` 内;`CALL`/协程切换点随 M6 补。

### 4.7 原生函数(ObjNativeFn)与侧信道错误寄存器

原生函数把一个 C++ 函数包成 aria `Value`,供 builtins(`print`/`len`/`type`/...)与未来嵌入 API 使用。`CALL` 命中 `ObjNativeFn` 时**不进字节码帧**,同步直接调用。其调用约定三件套配套设计,核心是把冷路径错误踢出返回类型--错误是少发的,不该位于热路径上。

**签名**

```cpp
using NativeFn = bool (*)(AriaVM& vm, Span<Value> slots);
```

- `vm`:单一宿主句柄(对标 Lua `lua_State*` / Wren `WrenVM*` / N-API `env`)。报错 `vm.fail(code, fmt, ...)`、分配 `vm.gc()`、未来回调 aria 函数均经它;M6 协程期 `vm` 路由到当前协程,故原生函数不持、也不需要 `VMContext` 引用--单句柄即可,且自动随当前协程。
- `slots`:调用区 `[callee, a1..aN]` 的可写视图(连续 `argc+1` 个槽)。`slots[0]` = 槽 0(callee / **返回槽**),`slots[1..argc]` = 实参 a1..aN,`argc = slots.size() - 1`。
- 返回 `bool`:`true` = 成功(返回值写 `slots[0]`,见下),`false` = 失败(已调 `vm.fail`/`vm.raise` 置寄存器,见下)。

**返回值就地写槽 0** -- 原生函数应能访问槽 0,把返回值直接写进 `slots[0]`(原地覆盖 callee)。VM 调用后 `ctx.drop(argc)` 弹掉 a1..aN,`slots[0]` 升至栈顶即返回值--比「`drop(argc+1)` + `push(result)`」省一压,且原生函数就地掌控返回槽。栈形与用户函数 `CALL` 一致(`[callee, a1..aN]`,callee 在槽 0),`peek(argc)` 即槽 0(`Movement::peek` 返回可写 `Value&`,无需新访问器)。

**错误走侧信道寄存器** -- 原生函数调 `vm.fail(code, fmt, ...)`(或 `vm.raise(err)`)写入 `VMContext` 的挂起错误寄存器(`Movement::pending_error_`)后 `return false`;`vm.fail`/`vm.raise` 均返回 `false`,故失败路径惯用一行 `return vm.fail(...)`(同时置寄存器与返回失败),成功路径 `slots[0] = ...; return true;`。VM 在 `CALL` 后以**返回的 bool 为成败信号**--`true` 走成功路径(`drop(argc)`,`slots[0]` 升至栈顶),`false` 经 `take_error()` 取出寄存器中的 `Error` 沿现有 `runtime_err` 路径传播(M1 即作未捕获错误从 `run()` 返回)。约 56B（libc++)/64B（libstdc++) 的 `Error` 仅在出错时构造,不进每次调用的返回值。寄存器置于**执行上下文**而非 `AriaVM`:错误状态随上下文走,M6 协程期每个协程有独立的挂起错误(各自 raise/检查,互不串扰);M1 单一主上下文,等价于 VM 级单寄存器。`reset()` 复用上下文时一并清空;`raise` 断言当前无挂起(防嵌套 raise 未取走就再 raise)。

**bool 与寄存器的同步** -- bool 是成败信号,寄存器是错误载荷容器,二者须一致。VM 据 bool 分支:成功路径仅 debug 断言 `!has_error()` 验证契约(寄存器本就空 -- 进场已守、原生未 raise,无需 clear_error;若违约 debug 暴露,release 不静默清掉掩盖),失败路径 `take_error()` 取载荷(寄存器空则 `*` 解引用空 Opt 属 UB,debug 断言先暴露)。debug 断言 `ok == !has_error()` 捕捉两类违约:
- 调了 `vm.fail` 却 `return true`(忘 `return false`):`ok=true ∧ has_error=true` -- release 下不再 `clear_error` 掩盖,残留错误随寄存器泄漏至下次调用(违约属实现 bug,任其表面化胜于吞掉);debug 断言先暴露。
- `return false` 却没调 `raise`(声明失败无载荷):`ok=false ∧ has_error=false` -- release 下 `take_error()` 取空、`*` 解引用空 Opt 属 UB;debug 断言先暴露。
**契约:`return false` ⟺ 已调 `vm.fail`/`vm.raise`;用 `return vm.fail(...)` 即自动满足。**

**与 raise(§4.5)的关系** -- 本寄存器是 CLAUDE.md「错误处理」第 2 条 `raise` 的 **M1 最小切片**:M1 无 try/catch,「raise」= 置寄存器 + 让 `run()` 返回;M3 落地完整 `raise` 时,在此寄存器基础上接 `find_try_handler` 查表 + `truncate` unwind + 跳 handler(逻辑见 §4.5),寄存器本身不变。即 M1 的侧信道寄存器是 M3 `raise` 的公共底座--原生函数的 `vm.fail` 与未来 op 处理器的 `raise` 共用同一寄存器。

**错误无位置** -- 与 M1 现有运行时错误一致(`runtime_err` 经 `Error::format` 构造,皆无位置,见 `Error.hpp`)。位置标注是跨切面的未来任务(适用所有运行时错误,经当前帧 `ip` 查 `CodeUnit` 行号表),不独压原生函数。

**不存 arity** -- 原生函数天然变参(对标 Lua/Wren/clox),fn 自查 `slots.size()` 做元数校验,不符 `vm.fail(WrongArity, ...)`。这与 `ObjFunction.arity_`(进帧布局需要、编译期定死)的不对称由调用约定正当化:`ObjFunction` 进帧需 arity 布局部槽,`ObjNativeFn` 不进帧、无需 VM 预校验。将来若要统一可上 `ObjCallable` 基类暴露 `Opt<u8> arity()`,但当前不上(YAGNI)。

**叶子调用契约** -- 原生函数不得操作 VM 值栈(`push`/`pop`/`drop`),否则 `slots` 视图失效(值栈增长会搬迁重定位,见 §4.1)。只读 `slots[1..]`、写 `slots[0]`、经 `vm.fail`/`raise` 报错。回调 aria 函数属未来机制(由 `vm` 提供,自管栈纪律)。GC 已启用(值栈/帧接根),原生函数内可经 `vm.gc()` 分配(`new_string`/`new_object` 等);跨分配持有的中间对象须 `Guard` 入临时根,`slots[0]` 写入后即随值栈为根。

**内建作者体感**(从 `Result<Value, Error>` 的啰嗦降到一行):

```cpp
bool len_native(AriaVM& vm, Span<Value> slots) {
    const auto argc = slots.size() - 1;
    if (argc != 1) { return vm.fail(ErrorCode::WrongArity, "len expects 1 arg, got {}", argc); }
    slots[0] = Value::from_int(compute_len(slots[1]));  // 就地返回
    return true;
}
```

**落地**:类型 `src/object/ObjNativeFn.{hpp,cpp}`;寄存器 `Movement::pending_error_` + `raise/has_error/take_error/clear_error`;`AriaVM::raise/fail` 返 `false` 转发到当前上下文;`call_value` 加 `ObjNativeFn` 分支(bool 成败信号 + 槽 0 返回 + 寄存器载荷)。`tests/test_ariavm.cpp` 4 例(槽 0 返回 / 零元 / 侧信道错误 / 元数自查)。注意:**原生函数类型与 CALL 路径已落地,内置函数注册机制亦已落地**(方案 B VM 级 builtins 表 + LOAD_GLOBAL 回退,见 §7)。

## 5. 早期简化(M1 的刻意收敛)

M1 目标只有一句话:**让一个手写/编译产出的 CodeUnit 在 VM 里跑完,值栈与帧栈行为正确**。刻意砍掉:

- **不继承 Object**(已接 GC 根):`Movement` 仍是 `AriaVM` 的纯 C++ 成员(非 Object),但值栈/帧已经 vm_roots tracer 接 GC 根(见 §4.6「已前拉」),`run()` 不再禁 GC,`JUMP_BACK` 已是 safe point。开发期即开 GC(stress GC 于集成测试)以早暴露缺失根。M6 升级 `ObjMovement : Object` 入对象链表 + open upvalue 链 + 多协程根并集。
- **无闭包/upvalue**:`CallFrame::closure_` 过渡期持 `ObjFunction*`;`CLOSURE`/`LOAD_UPVALUE`/`STORE_UPVALUE`/`CLOSE_UPVALUE` 暂 pass。
- **无完整异常**:`raise` 的完整形态(`TryRecord` 查表 + `truncate` unwind + `THROW`)暂不实现;op 失败直接作为 `run()` 的失败返回。但 `raise` 的 M1 最小切片--`VMContext` 上的挂起错误寄存器--已随原生函数落地(见 §4.7),供原生函数侧信道报错;M3 完整 `raise` 在此寄存器上接 unwind,寄存器本身不变。
- **无模块/类/导入**:globals 暂以 VM 内单张表顶替(M2 换 per-module)。
- run() 不可重入问题不存在(M1 无 native 回调、无协程),但**循环结构从一开始就按可重入写**(状态全部取自 `*current_`,循环体无静态/成员临时),避免将来返工。

## 6. 实施路线

| 阶段 | 内容 | 验收 |
| :--- | :--- | :--- |
| **M1 跑起来(已落地)** | `Movement`(纯 C++ 类)+ `VMContext` 别名;`CallFrame`(持 `ObjFunction*`);`AriaVM::run()`:`LOAD_CONST/LOAD_IMM/LOAD_NIL/TRUE/FALSE`、局部槽(含 `_L`)、算术/比较/`NOT`/`NEGATE`、`POP/POP_N/DUP/DUP2`、`JUMP*`/`JUMP_BACK`、`CALL`(`ObjFunction` 进帧 + `ObjNativeFn` 同步调用,见 §4.7)、`RETURN`、`HALT`、`PRINT`。值栈可增长;`VMContext` 挂起错误寄存器(§4.7,M1 `raise` 切片) | 手写字节码算术/循环/函数调用/原生函数跑通,ctest 371/371 绿(M1 当时快照) |
| **M2 全局与模块** | `ObjModule`、模块表、`DEF/LOAD/STORE_GLOBAL`、内置函数注册机制(指令集 §6.4 待决项在此定) | 模块顶层 var/fun 可定义可读 |
| **M3 异常** | `TryRecord` 扩字段、`raise`、`THROW`、truncate unwind、finally 语义细化 | try/catch 单测,跨帧 unwind 正确 |
| **M4 闭包** | `ObjClosure`/`ObjUpvalue`、`CLOSURE`、open upvalue 链、`CallFrame::closure_` 换闭包。open upvalue 落地后须在值栈增长时重定位其 Value*(或改索引式) | 计数器闭包等经典样例正确 |
| **M5 类与对象** | `ObjClass`/`ObjInstance`/`ObjBoundMethod`、`MAKE_*` 系列、bootstrap Object 根类、`init` 缓存(指令集 §5.5) | 类定义/实例化/继承/super 样例通过 |
| **M6 协程 + GC 根** | `Movement` -> `ObjMovement : Object`(重命名 + trace + `ObjType::MOVEMENT`)、`VMContext` 别名指向之、GC mark 回调 + safe point、`YIELD`/`RESUME`(指令集新增,`CALL` 语义扩「callee 是协程则 resume」)、调度列表 `contexts_` | 协程生成器样例;stress GC 下多协程无悬垂 |

顺序依赖:M4 依赖 M1 的帧/栈;M5 依赖 M4(方法即闭包);M6 依赖全部。M2/M3 可与 M4 并行。字节码编译器(AST->CodeUnit)已落地(CodeGen,42 个 visit);M1 早期曾以手写 emit 测试驱动 VM,现由 CodeGen 产出字节码。

### M1 验证状态

- `aria_tests` ctest 371/371 全绿(`tests/test_ariavm.cpp` 覆盖:算术/f64 常量与提升/while 循环跳转回填/函数调用与栈复原/真值与短路/相等语义/TypeMismatch/DivisionByZero/WrongArity/失控递归 StackOverflow/NotImplemented,以及原生函数 4 例:槽 0 返回 / 零元 / 侧信道错误 / 元数自查)。
- **局部区预留约定**(见指令集文档 §4.3):帧的 `slots` 指向槽 0(callee),槽区不自动保留,函数序言必须先压 nil 预留全部局部槽,否则首个临时值覆写槽 1。
- **短路跳转的 L_end 是 `<b>` 之后的汇合点**(非 `<b>` 之前);跳转偏移以读完操作数后的 ip 为基准(与 Disassembler 解码一致),手写回填需按此计算。
- M1 暂定语义(待语义阶段定,指令集 §9):int/int 截断整除、除/模零报运行时错误、f64 按 IEEE(除零得 inf/nan)、真值为 Lua 风格(仅 nil/false 为假)。

## 7. 缺口清单(实施前需补的东西)

- **`YIELD`/`RESUME` 指令缺失**(指令集文档需增补,参照 `INVOKE_METHOD` 的「预备指令」先例)。
- **`ObjType` 无 `MOVEMENT`**(M6 增)。
- **`TryRecord` 字段不全**(缺 stack_depth/frame_depth/catch_slot,M3 补)。
- **GC 的 VM 根回调接口**已落地(`GC::set_vm_roots`,AriaVM 构造期注册,标 `modules_`/`builtins_`/值栈/帧)。
- **内置函数注册机制**已落地(方案 B「VM 级 builtins 表 + LOAD_GLOBAL 回退」,无新指令):`src/runtime/Builtins.{hpp,cpp}` 的 `builtins::register_builtins(GC&, AriaHashTable&)` 把 `type`/`len`/`str`/`assert` 等原生函数经 `new_native_fn` 包成 `ObjNativeFn` 后按名 `upsert` 进 AriaVM 的 `builtins_` 表(VM 级 `AriaHashTable`,全 VM 共享一份;`print` 是关键字/语句走 `PRINT` 指令,不入此表)。注入点唯一:AriaVM 构造期 `set_vm_roots` 之后调用一次,全 VM 生命周期共享,不再每模块注入。`LOAD_GLOBAL` 先查当前模块 globals,miss 回退 `builtins_`(Python 式 globals -> builtins 查找链);`STORE_GLOBAL` **不**回退 builtins(赋值不隐式创建,必须先 var 声明,见 grammar.txt §205-206),仅 `DEF_GLOBAL` 写模块 globals 可 shadow 内置。intern 池保证 CodeGen 发射 `LOAD_GLOBAL "name"` 与注册名同指。原方案 A「按模块预填 globals」会在 REPL 逐行 `run()` 重注册、覆写用户 shadow(与「顶层 var 跨行保留」矛盾),方案 B 一份只读表彻底回避,并省掉每模块 4 个 `ObjNativeFn` 分配。原生函数**类型与 CALL 路径**早已落地(见 §4.7)。
- **VM 与 GC 的拥有关系**:已定 -- VM 拥有 `GC gc_` 值成员(每 VM 一个 GC,REPL 常驻)。

## 8. 参考

- `.claude/reference/bytecode/bytecode-instruction-set.md`:指令集/操作数编码/栈效应/lowering(VM 实现的操作语义基准)。
- `.claude/reference/memory/gc-implementation-plan.md` §5 Phase 4:Movement + VM 根 + safe point(本文 §4.6 的细化来源)。
- `src/runtime/FrameStack.hpp`:帧栈模板 + `truncate`(unwind 用)。
- `src/bytecode/CodeUnit.hpp`:`TryRecord`/`find_try_handler`(异常查表已就绪)。
- CLAUDE.md「错误处理」第 2 条:VM 自管异常的设计目标。
