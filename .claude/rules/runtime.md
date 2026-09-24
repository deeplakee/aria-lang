---
name: aria-runtime
description: aria 解释器 runtime 层模块参考：FrameStack、Movement 执行上下文（值栈/帧栈/开 upvalue 链/挂起错误寄存器）、AriaVM（主循环/闭包与 upvalue 指令/全局与 builtins 回退/IMPORT 模块加载，含 VM 异常通道落地状态清单）。读写 src/runtime/**、实现 VM 里程碑（M3 异常、M4 闭包等）时使用。
paths:
  - "src/runtime/**"
---

# runtime 层模块参考

VM/执行上下文的设计与分阶段路线见 `.claude/reference/runtime/vm-design.md`；import 端到端处理见 `.claude/reference/runtime/import-handling-overview.md`，路径解析细节见 `.claude/reference/runtime/import-path-resolution.md`；M3 异常（try/catch/throw）的踩坑归档与对策见 `.claude/reference/runtime/exception-implementation-pitfalls.md`（finally 不支持、后继 defer 为可选后续，异常相关特性重启前重读）；方法派发/迭代协议的成本基线与测量手法见 `bench/vm_bench.cpp` 文件头与 `bytecode-instruction-set.md` §6.2（动 VM 派发路径前先读）。均按需 Read。

## `runtime/FrameStack.hpp`

`FrameStack<T, Capacity>` 模板--面向 trivial 帧（`CallFrame`）的栈式槽位池。

- 零开销（`acquire` 即返回槽引用 + 计数自增），一次分配永不扩容（指针绝对稳定）。
- `truncate(n)` 供异常 unwind 跨多帧（按异常记录表登记的深度一步回退）。
- 要求 T trivial + trivially-copyable + trivially-destructible（`static_assert` 三连把关）；GC trace 经 `span()` 读已用区间。
- 与 `Array<T>` 不通用：`Array` 是堆背书、可扩容、非 trivial（持 `GC*`、不可拷贝/移动），`FrameStack` 是定容槽位池；语义不同故不套用。

## `runtime/Movement.hpp`

执行上下文（纯 C++ 类，非 Object：值栈/帧不进对象链表，改经 AriaVM 的 vm_roots tracer 在 collect 时直标，M6 协程期升级 `ObjMovement : Object` 入链表；`using VMContext = Movement` 别名）。

### 值栈与帧栈

- 可增长值栈：GC 分配的 `Buffer<Value>` 底座 + `top_` 裸指针，初始 `kStackInit = 1024`，push 溢出 2x 增长并重定位活动帧 slots 与 open upvalue 链 location_。
- 值栈 API `push`/`pop`/`drop`/`peek`/`stack_size`/`stack_capacity`/`stack_base`/`stack_top`；帧栈 `frames()`/`frames_full`。
- `FrameStack<CallFrame, kFrameMax = 256>`；私有 `set_stack_top_` 收口「值栈顶只由 Movement 自身改」。
- 进/出帧 `enter_frame(closure, argc)`/`exit_frame()` 收口「栈顶帧 slots 即值栈本帧槽 0」；`exit_frame` 内置 `close_upvalues(frame.slots)`（弹帧 + 关本帧区间开指 + 复位）；`enter_frame` 定义在 `.cpp`（需 `ObjClosure` 完整类型）。
- **方法帧共用本入口**：调用方进帧前把槽 0 原位写为 this--普通帧槽 0 = 闭包自身、方法帧槽 0 = this，闭包经 `frame.closure` 携带不上栈（对齐 clox 方法语义；无专用方法进帧）。
- **`unwind_to_handler(n, record)` 异常派发**：`truncate(n+1)` 一步弃内层帧 -> `close_upvalues(catch 槽)` 按槽址一关到底（此时栈顶未动、槽区全存活）-> 栈顶截到 catch 参数槽 -> 置 ip 跳 handler -> 寄存器载荷 push 落槽。
- `reset()`：先关全部开指再清场--HALT 收场不弹帧的安全网，防开链跨 run 复用同一栈区读脏值。

### 开 upvalue 链（定义在 `.cpp`）

链头 `open_upvalues_`：按槽址降序的侵入式单链，链上节点经 VM 根 tracer 标根。

- **`capture_upvalue(slot)` 捕获单点**：单趟同时完成复用判定与降序插链--等值即复用（同槽同局部一份引用，「捕获即引用」内外层共享同一 `ObjUpvalue`），更小/链尾则经调用方传入的 GC `new_upvalue` 建新插链（Movement 不自持分配器创建对象）；建新到插链间无分配点，入链即经 tracer 根化；同槽双节点也命中复用而非再插（自愈）。
- **`close_upvalues(from)`**：闭所有 `location >= from`（迁值 close + 整段摘链，降序不变式下恒为链头前缀）。
- 只读链头 `open_upvalues()` 供 tracer 遍历标根。
- **关闭挂点**：`exit_frame`（本帧区间，RETURN 经此）/ `CLOSE_UPVALUE`（`stack_top()`）/ `unwind_to_handler`（catch 槽）/ `reset()`（全链）。

### 协程 resume 链字段 `previous_`

`previous()`/`set_previous()`--M6 前置落地：主上下文恒 nullptr 链尾，VM 根 tracer 自 `current_` 沿链逐个标根，「resume/yield 严格成对」的切换纪律由链尾断言锁定；**M6 前链长恒 1**。M6 定稿单循环切换模型后 `previous_` 对齐 Wren caller 语义（yield/完成解链、允许再 resume），链尾断言退役（执行链遍历保留--main_ctx_ 不入堆），见 vm-design.md §4.9。

### 挂起错误寄存器

`Opt<Value> pending_error_`--运行期异常主通道寄存器（单寄存器模型，见 `exception-implementation-pitfalls.md` 坑 #7）：

- VM 检测错误/原生报错装箱 `ObjException`（码 + 完整烘焙消息）后写入；用户 `throw` 写原值。
- `raise(Value)` 收已构造好的载荷，构造（需分配）在持 GC 的调用方完成；`raise` 断言当前无挂起（防嵌套）。
- 只读访问器 `pending_error()` 供 VM 根 tracer 标根（置入到 take 之间跨安全点分配不回收）。

### `CallFrame`（trivially-copyable 聚合，同文件）

- `closure`（callable 收敛为闭包，顶层入口也闭包）、`unit`（缓存 `closure->function()->unit()`，省每条指令一跳）、`module`（缓存供 `LOAD/STORE/DEF_GLOBAL` 定位模块 globals）。
- `ip`（裸指针）；`slots`（局部基址，callee 在槽 0、参数从槽 1 起）。
- `last_ip`（本帧最近取指指令起始指针，主循环取指前写/`init_frame_` 置 code 起始）：运行期报错定位行号与 unwind 查表共用锚点，按 `last_ip - unit->code.data()` 反推 offset（前提：帧存活期间 code 缓冲恒定--非移动 GC + 执行期零 emit），无 NSDMI 保 trivial 聚合。
- 模块体 run-once 帧不另设标志位：RETURN 按函数名 == `<module>` 判定（导入模块入口名固定为 `<module>`，用户代码产生不出含 `<>` 的名字；`kModuleEntryName`，见 aria.hpp）。

### `grow_stack_` 三类指针重绑

定义在 `.cpp`（链重绑解引用 `ObjUpvalue` 需完整类型）：

- 搬运**前**记相对 old_base 的整数偏移--`top_`/各帧 `slots` 走栈内定长数组，开链 `location_` 节数不定、走 `List<usize>` 暂存（链序两趟间稳定，免数节点一趟）。
- 搬运**后** `new_base + 偏移` 重建，从不触碰 dangling 指针（对 dangling 指针做指针减法是 UB）。

## `runtime/AriaVM.hpp` / `.cpp`

解释器（M1-M5 已落地；M6 协程待）。循环状态全部取自 `*current_`。

### 执行入口

- **`run()` 返 `Result<Value, Error>`**（成功 = 返回值，失败 = 未捕获运行时错误；M6 协程挂起将扩三态含 `Yielded`）。两个执行入口：`run(ObjFunction*)`（执行已编译函数）；`run(SourceFile&, ObjModule*)`（**编译并执行**：经静态 `Compiler::compile` 编进 `module` 入口，编译失败原样透传首错；`source` 须存活到 `compile()` 返回，`module` 由编译期 `make_guard` 根化）。
- **turnkey interpret 入口**返回 `InterpretResult` 枚举（`Ok`/`CompileError`/`RuntimeError`/`LoadError`，对齐 clox）：`interpret_from_src`（合成 `<script>` 模块）/`interpret_from_path`（`SourceFile::from_path` 读盘，失败或 name 空返 `LoadError`）。
- interpret 内部渲染错误到 stderr 后只回类别--`Error` 已自有位置串、与 `SourceFile` 解耦，故 turnkey 入口内部构造/读盘的 `SourceFile` 返回后销毁也不悬垂，无需 VM 源注册表。共用私有 `interpret_run` 按**失败阶段**分类（编译期失败 -> `CompileError`；run 期失败 -> `RuntimeError`。不按错误码大类映射：运行期才抛的 `UndefinedVariable` 与经 IMPORT 异常通道传播的**被导入模块编译期错误**（主模块已在执行、可被 try/catch 捕获）均归 `RuntimeError`）。

### `run(ObjFunction*)`：程序入口仪式

- 入口断言 `current_ == &main_ctx_`（M6 前恒真；M6 单循环切换模型下升格为永久不变式--`run()` 是唯一驱动入口、resume/yield 不重入）。
- 源根入口槽 `[0]` 原地替换为入口模块 `dir_` + 前后各一次 `reset()` 清场（前：HALT 收场的上一轮不弹帧，不先清场会把新帧叠在陈旧帧上；后：避免 `run()` 外 GC 经 tracer 标到陈旧栈值）。
- 入口 fn 包空闭包（顶层也闭包，M4；fn 跨 `new_closure` 的 GC 点 `make_guard` 兜底），委托私有 `run_closure(closure)`。

### `run_closure(ObjClosure*)`：执行本体

- 执行本体，无入口装饰：压 callee + 经 `call_closure` 进帧（进帧单点收束，与运行期调用同路；帧满失败分支为重入接缝承重，失败拆寄存器载荷直转 `Result`、不经 unwind）-> `dispatch_loop()` 主循环，作用于 `*current_`。
- 未来**重入**的接缝：指令执行中临时执行一个闭包（原生回调调 aria 函数 / 嵌入宿主调函数，见 vm-design.md §4.7）经此进入，故不播源根、不 reset（重入调用者的栈不可冲掉）、不断言主上下文；落地时升公开，尚欠 dispatch_loop 按基线帧深退出与实参布线。

### GC 已启用

值栈 `[base, top)` + 各活动帧 `closure`/`module` + open upvalue 开链经 VM 根 tracer 标根（见下「共享状态」）；`IMPORT` 取到模块对象后 `current_->push(module)` 压栈（经 `modules_` 根可达，非移动 GC 故指针稳定，**不加守卫**）；`JUMP_BACK`（循环回边）为 safe point 调 `gc_.maybe_collect()`。

### 指令子集

指令 case 与 `code.hpp` 表行同批落地（不存在「表里有、VM 没实现」的持久态），未设 case 走 `UNREACHABLE`（`fatal_error`）；各指令的栈效应与逐 case 语义见 `AriaVM.cpp` dispatch_loop（case 注释即契约）。本层只需记住的跨文件约定：

- **全局**：`LOAD_GLOBAL` 先查模块 `globals_`、miss 回退 VM 级 `builtins_` 表（Python 式查找链，内置 type/str/println/assert 经此解析），再 miss 报 `UndefinedVariable`；`STORE_GLOBAL` 仅写模块 `globals_`、**不**回退 builtins（赋值不隐式创建，必须先 var 声明，见 `docs/grammar.txt`「作用域模型」裸名赋值条）。
- **算子取实现**：九个二元算子共用执行体 `run_binary_operator<Op>`，非对象左值委托 `run_binary_numeric<Op>`，对象左值经 tag 判定取本对象的 `Object::op_*_impl` 再 `call_value`（调用区 `[lhs, rhs]` 即 `[this, arg1]`）；取不到的措辞随宿主。`+` 与四个比较算子的域 = 数值 ∪ 侧为 String（String 的 5 个 override 直给实现格，拼接经驻留池、比较按无符号字节序，见 object.md ②）。
- **相等/栈操作/跳转**：`EQUAL`/`NOT_EQUAL` 走 `value_equal`、`STRICT_*` 走 `value_identical`；`JUMP*` 为 u16、方向在 opcode、偏移以读完操作数后 ip 为基准，含 `JUMP_TRUE_OR_POP`/`JUMP_FALSE_OR_POP` 短路。
- **`CALL` 族**：callable 收敛为闭包（`ObjFunction` 退为常量池内部物），`call_value` 编排后按 callee 类型分发到 `call_closure`/`call_native`/`call_class`/`call_bound_method`，其余对象类型按调用钩子 `__call__` 取实现后递归分发。
- 三个 `call_*` 均不收 ctx 参数、作用于 `*current_`（直接读 `current_`，与 dispatch_loop/raise 语义统一），返 `bool` 成败：失败时错误载荷已 `raise` 进 `*current_` 挂起寄存器，调用方据 bool 调 `unwind()`。
- **`RETURN`**：弹返回值 + `exit_frame`（内置关本帧被捕获局部，值迁入各自 upvalue 自持）；顶层主帧返回即返回程序结果，模块体帧（按函数名 == `<module>`）则弹弃返回值、改压模块对象。
- **闭包与 upvalue**：`CLOSURE` 取常量池 `ObjFunction` 现场包闭包并**立即压栈**（「栈即根」，跨捕获循环免守卫）；`is_local` 槽址经 `capture_upvalue` 单点复用/新建插链，`false` 穿透复制外层 upvalue；`LOAD/STORE_UPVALUE` 经 `value_slot()` 开/闭两态同址不分叉；`CLOSE_UPVALUE` 批量关槽址 >= 栈顶的开 upvalue、无弹栈（对齐 Lua `OP_CLOSE`，编译器在弹区 `POP_N` 后发射）。
- **类与对象 bootstrap**：ctor 期 `bootstrap_registers()` 编排 + `bootstrap_object_class()`（Object 根类 + 原生 no-op init，init Value 化 `return true` 不写槽、保 ObjFunction「module 恒非空」不变式）；List/Iterator/Map/String/Range bootstrap 类（`ObjClass(super=Object 根)`）各经 `register_*_builtins` 装方法面（住 `runtime/builtins/`，方法面是 VM 侧语言面、object 层保持纯表示），入各值寄存器格。
- **八指令**：`LOAD_REG`（压只读值寄存器）、`MAKE_CLASS`（peek super 不弹跨分配；非类值语言可达报 `TypeMismatch`）、`MAKE_METHOD`/`MAKE_STATIC`（经 `ObjClass::set_field` 落接收类表；MAKE_METHOD 仅实例方法仅收闭包、戳 defining class 一职双任 = super 来源 + 方法性标记，MAKE_STATIC 收静态变量与 fun 静态方法不戳）、`LOAD_FIELD`/`STORE_FIELD`（命名成员读/写统一走 `Object::load_field`/`store_field` 协议，执行体只透传信号）、`LOAD_THIS_FIELD`/`STORE_THIS_FIELD`（case 内直调协议不设执行体，this 取帧槽 0 不经栈，编译器不变式 + ASSERT 钉）、`LOAD_SUPER_FIELD`（defining class 从方法闭包直读，从父类起沿链查不含自身）、`PREPARE_METHOD`/`CALL_METHOD`（见下）。
- **方法两段式**：`recv.name(args)` 一律 `PREPARE_METHOD` + `CALL_METHOD`（解析先于实参求值，经 `Object::load_field_unbound` 不绑定取被调值；CALL_METHOD 把调用区整成 `[recv, a1..aN]` 后交 `call_value`），与 `call_value` 逐位同形；`super.m(args)` 不经两段式。
- **集合**：`MAKE_LIST`/`MAKE_MAP` 元素/键值 peek 在栈跨 `new_*` 顶部（「栈即根」）、整段拷入走 trivial 分配不触 GC；下标族 `LOAD_INDEX`/`STORE_INDEX` 统一走 `Object::load_index`/`store_index` 协议（list 整数键从尾计数 + Range 键切片、map 任意键 miss `KeyError`、string 字节键）；`MAKE_RANGE` 端点须整数（非整数 `TypeMismatch`）。
- **值填槽**：局部槽由编译器按「值填槽」填充（声明不预占，初始化器值 / 无初始化器时的 `LOAD_NIL` 恰落槽位，见指令集文档 §4.3）。**真值** Lua 风格（仅 nil/false 假）。

### `IMPORT` 与 `load_module`

- **`IMPORT`**（执行体私有 `run_import`，bool 契约同 `call_*` 族，unwind 留 dispatch_loop 调用点）：`resolve_module` 解析 specifier 为绝对规范路径（模块表键）-> intern -> `modules_` 查表：命中（体待 run-once / 循环导入跑中 / 已跑完）复用并压栈；未命中 `load_module` 得模块，以其 `entry` 现场包空闭包经 `call_closure` 进帧后 break--**模块体 run-once 即执行一个函数**，由主循环照常驱动，其 RETURN 按函数名 == `<module>` 判定模块体帧、弹弃返回值压回模块对象，故命中/未命中栈效应统一 `[..., module]`，**无递归 `dispatch_loop()`**。解析失败（`resolve_module` 返 `nullopt`）报 `ModuleNotFound`。
- **`load_module(canonical_path, import_specifier)`**（私有，**仅加载与编译**、**仅限 dispatch_loop 驱动期调用**（寄存器随 `*current_` 走，run 外直调错误会被吞））：读盘 -> 派生身份（`fs::module_name_and_dir`，同入口约定）-> `new_module` + `make_guard` -> `Compiler::compile(..., kModuleEntryName)` -> **编译成功才 `modules_.set` 入表**（加载事实源 = 表成员资格，供循环导入命中体执行中的半初始化对象；失败一律不留表项、同路径重试重新加载）。`canonical_path` 一身二任：`modules_` 表键 + 读盘路径。
- **错误契约同 `call_value` 族**：返 `ObjModule*`，失败 `nullptr ⟺` 载荷已 `raise`；读盘失败/name 空经 `fail` 报 `ModuleNotFound`（带 IMPORT 站点位置），编译期 `Error` 就地 `new_exception` 原样装配箱透传（含被导入文件位置，不重烘）。
- **根安全**：`run_import` 的 `gc_.make_guard(canonical_path)` 跨 `load_module` 内一串 `new_*` 分配根化（intern weak root 不保命）；`modules_.set` 等表 rehash 走 trivial 分配不触 GC、不是守卫承重点；`module`/`entry` 经 `modules_` + `module->entry_` 根可达。详见 `import-handling-overview.md`/`import-path-resolution.md`。

### 共享状态

- 自有 `GC gc_`（值成员，每 VM 一个）+ 主上下文 `main_ctx_` + 当前执行上下文 `current_`（构造指 `&main_ctx_`；dispatch_loop/`call_value` 族/raise 的作用对象。M6 单循环切换模型：resume/yield 为原生函数、CALL 善后点换指、dispatch_loop 永不重入，任何时刻正在执行的字节码所在上下文恒等于 `current_`）。
- 模块表 `modules_`（`AriaHashTable`，键 = 规范路径 `ObjString*` intern、值 = `ObjModule*`，均装箱为 `Value`）。
- VM 级只读 builtins 表 `builtins_`（构造期由 `builtins::register_builtin_functions` 一次性填充 type/str/println/assert，全 VM 共享，`LOAD_GLOBAL` 模块 globals 未命中后回退查此）。
- 源根列表 `source_roots_`（`List<String>`，`[0]` = 入口槽 cwd 占位/`run()` 换入口 `dir_`、`[1..]` = 配置根 stdlib/`-L`）与值寄存器组 `registers_`（`List<Object*>`，VM 单例对象的统一存放表，构造期按表长预置格、bootstrap 按 `k<名字>Offset` 具名格位填、填完经 `assert_slots_filled` 收口，注册表见 `runtime/value_register.hpp`，寄存器只读）。
- 常量串表 `string_constants_`（`List<ObjString*>`，VM 自己按名取用的字符串常量的唯一存放处，注册表见 `runtime/string_constant.hpp`）：构造期按表长预置格、bootstrap 按下标（枚举值）逐格 `new_string` 填入，填完经 `assert_slots_filled` 收口；tracer 一趟 `mark_object` 标根--**驻留池是 weak root，不标根则下轮 collect 即摘除**（算子钩子名尤其如此：实例算子派发每次都要一个稳定的 `ObjString*`，不标根就退化成每轮重铸）。消费点经 `string_constant(StringConstant)` 取值，不再各自 `new_string`。注册表的**成员判据**：只收 VM 自己按名取用的串--代码里写下的常量名（字段/方法名）不进此表，那些编进常量池经 `ObjFunction::trace` 已可达。
- 构造时把 VM 根 tracer 经 `gc_.set_vm_roots` 注册进自有 GC（组合而非继承：GC 不识 VM 类型），于构造临界区（`make_lock` 挂起 GC，窗口内创建免守卫、解锁前对象须全部发布进 tracer 可达之家）内 bootstrap 常量串表 + registers 并注册 builtins（常量串表须先于 registers：String 类 bootstrap 末段的算子钩子缓存按名取串，读的就是本表）。
- **collect 时标五类根**：① `modules_`（进而 trace 各模块 `name_`/`dir_`/`entry_`/`globals_`）；② `builtins_`；③ `registers_`（一趟循环逐格 `mark_object`）；④ `string_constants_`（一趟循环 `mark_object`）；⑤ `current_` 执行链（自 `*current_` 沿 `previous_` 走到链尾，挂起协程的值栈/帧/寄存器皆根；链尾断言 == `&main_ctx_` 锁切换纪律；M6 定稿后链尾断言退役、链遍历保留）。
- 逐上下文标：值栈 `[base, top)` 全部 Value（run() 期局部/实参/临时值只活在栈上，最关键的根）；各活动帧 `closure`/`module`；挂起错误寄存器；open upvalue 开链（「闭包已死而 upvalue 仍在链」的悬垂防线）。以 tracer 直标代替 Movement 升 Object（M6 升级 `ObjMovement : Object` 入链表）。
- **`raise(code, fmt, args...)`/`fail`**：从零构造消息一步烘齐（`Error::make_message` 无位置版 + `new_exception`），消息**不含位置前缀**（位置由 unwind 未捕获出口的逐帧 at 行给出）；`fail` = raise + `FailSignal`（`[[nodiscard]]` 强制 `return vm.fail(...);`）。公共访问器 `gc()`/`main_context()`/`modules()`/`source_roots()` 供原生函数与测试用。

### 执行跟踪 `DEBUG_TRACE_EXECUTION`

宏由 CMake `ARIA_DEBUG_TRACE_EXECUTION` option 控制（OFF 默认，对齐 `ARIA_DEBUG_GC`/`ARIA_DEBUG_PRINT_CODE`），控制是否在每条指令执行前打印执行状态：`dispatch_loop` 主循环顶取 opcode 前（此时 `frame.ip` 指向待执行指令）调匿名 `trace_execution(*current_)` 经 `Disassembler::disassembleInstruction` 解码。三行输出到 stderr（与 GC 调试日志同走 stderr，与 `println` 的 stdout 分流）：① `[trace] <module name>  <fn名> @ip偏移 指令反汇编`（第一行即含完整位置上下文）；② `stack[n]: [ v1 ][ v2 ]...`（值栈 `[base, top)` 全部 Value 经 `format_value_debug` 渲染，空栈 `(empty)`）；③ `^ frame[i]`（`^` 对齐到当前帧 bottom 槽 `[` 下标，联动指示栈中哪一段是当前帧的局部区；fn/ip 已在字节码行不重复）。**`format_value_debug` 不用 `format_value`**：后者对 Obj 走可重载的虚 `Object::to_string()`，在 `dispatch_loop` 内逐指令调用会重入 VM 致无限递归；故对 Obj 走虚 `debug_repr()`（override 契约 = 纯 C++ 惰性渲染，绝不执行 aria 字节码 / 不触 GC，见 `Object.hpp`）。函数常态编译，关闭时无调用点、零开销。

## VM 异常通道（throw/catch）

aria 语言的 `throw/catch` 与 VM 检测到的运行时错误统一走 VM 自管机制（不引入 `SETUP_EXCEPT`/`END_EXCEPT`，不依赖 C++ 异常），设计全文见 `vm-design.md` §4.5-§4.8/§7，踩坑对策见 `exception-implementation-pitfalls.md`（坑编号 #1-#20）。

- 错误站点 raise 载荷入挂起寄存器 -> `AriaVM::unwind()` 自最内帧向外以 `last_ip`（指令起始，非已推进的 `ip`）反推 offset 查 `CodeUnit::try_records`（`find_try_handler`，嵌套取最内层），纯搜索不动帧栈/值栈（未命中帧记跟踪三元组）。
- 命中（循环内就地提前返回）：`unwind_to_handler(命中帧索引, record)` 一体完成弃帧+截 catch 槽+ip 跳+载荷落槽（见 Movement 节），载荷落 catch 参数槽（恒 == stack_depth，值填槽无 `STORE_LOCAL`）。
- 全帧未命中：`reset()` 一次清场后从未捕获出口物化 `Error`。
- **`unwind()`** 负责查表派发 + 未捕获物化（经 `AriaVM::take_uncaught_error` 反提拆 (码, 烘焙消息)：ObjException 直取原码原消息；非 ObjException 载荷兜底 `UncaughtException`）与堆栈跟踪烘焙，拼好后经 `Error::from_baked` 一次物化。`Error` 仅在 unwind 未捕获出口物化，与 AGENTS.md 通道 2 一致。

### 构成件

- `OpCode::THROW`（dispatch_loop 弹值 `current_->raise(v)`，原值入寄存器不包 ObjException--catch 绑原值保类型、未捕获物化兜底 `UncaughtException`）。
- `CodeUnit::try_records` + `find_try_handler`（`TryRecord{begin, end, handle, stack_depth}`，按 begin 非降序二分 + 前溯，begin 相等（内层 try 是外层体首条语句）由反向扫描天然取最内层）。
- `Movement::unwind_to_handler`（回退+转入 handler 一体）。
- `AriaVM::raise`/`fail`（装箱点一步烘消息、不含位置前缀）与 `unwind`（private，仅 dispatch_loop 驱动期，入口断言寄存器非空）。
- `ObjException`（携码 + 完整烘焙消息，re-throw 保码；`to_error` 经 `Error::from_baked` 原码原消息回传供测试/嵌入方）；VM 根 tracer 标 `pending_error_`（raise 到 unwind 间跨安全点不回收）；CodeGen 侧见 compile.md。

### 闭环与无 Result 直报形态

- dispatch_loop 内全部运行时错误站点统一走寄存器（无 `Result` 直报形态）；raise 与 unwind 不融合成 `*_and_*` 助手，站点就地两步、与 `CALL` 失败善后同形。各站点 `unwind` 返 somed `Error` 即 `return runtime_err(std::move(err))` 出栈，返 `nullopt`（命中 handler，帧栈已 truncate）则 `break` 回循环顶重取帧（switch 即整个 while 体；**switch 之后不得新增引用 frame 的代码**，该前提钉在 dispatch_loop 循环顶注释）。
- 9 个算术/比较 case 经 `run_binary_operator<Op>`（bool 契约同 `call_value`）；`NEGATE`/`LOAD|STORE_GLOBAL` 解析失败经 `raise` 就地装箱，`IMPORT` 解析失败在 `run_import` 经 `fail` 同源装箱返 false，`THROW` 弹值存原值，`CALL`/`load_module` 失败载荷已在寄存器，随后一律 `unwind()` 查表派发/物化。

### 未捕获堆栈跟踪（坑 #16）

- `unwind` 搜索阶段对未命中帧记 `TraceEntry{fn, mod, ip_off}`（搜索不动帧栈，帧引用全程有效），全未命中时反转为外 -> 内（Python 式 most recent call last）逐帧烘焙 `\n  at <fn名> (<loc>)` 进 `Error.message_` 尾部。
- 行号源 = 各帧 `last_ip` 查 `line_for_offset`（位置串 = `ObjModule::format_location(line)`，文件模块取 `abs_path` / 合成模块退化为 `<name>`，见 object 层「模块身份」）--跟踪行是运行期错误的唯一位置标注（消息不含位置前缀）。透传的被导入模块编译期 `Error` 不经 unwind，无跟踪。

### finally 不支持

善后后继 defer 已降为可选后续、不绑定里程碑：`finally` 回归普通标识符；try 无 catch 报 `TryWithoutHandler`（消息 `'try' requires a catch clause`）。
