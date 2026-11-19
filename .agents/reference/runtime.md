---
name: aria-runtime
description: aria runtime 层模块参考：AriaVM 宿主、AriaExeContext 执行上下文、ObjMovement GC 对象壳、Eval 驱动，以及调用、异常、协程与根追踪的跨文件契约。
paths:
  - "src/runtime/**"
---

# runtime 层模块参考

本文件记录状态归属、入口契约和跨文件连接。接口细节见头文件，指令编码与栈效应见
[指令集规格](specs/instruction-set.md)，import 路径规则见 [import 规格](specs/import.md)。
设计取舍见 [VM 笔记](../notes/runtime/vm.md)，异常与协程的失败原因、定序依据分别见
[异常笔记](../notes/runtime/exception.md)和[协程笔记](../notes/runtime/coroutine.md)。

## 文件与状态归属

| 文件 | 职责 |
| --- | --- |
| `runtime/AriaVM.hpp` / `.cpp` | 持 GC、共享服务表、当前上下文，提供执行入口和模块加载 |
| `runtime/AriaExeContext.hpp` / `.cpp` | 值栈、帧栈、开 upvalue 链、错误寄存器、恢复者链与重入边界 |
| `runtime/ObjMovement.hpp` / `.cpp` | 以 `Object` + `AriaExeContext` 多继承提供 GC 对象身份与 trace |
| `runtime/FrameStack.hpp` / `runtime/CallFrame.hpp` | 定容帧存储与单帧数据 |
| `runtime/Eval.hpp` / `.cpp` | 指令循环、调用、显示渲染和异常派发 |
| `runtime/NativeCallInfo.hpp` | 原生调用的实参、接收者与返回槽视图 |
| `runtime/builtins/` | 全局内建与类型方法面，注册规则见 [object.md](object.md#内置类型的成员面) |
| `runtime/value_register.hpp` / `runtime/str_table.hpp` | 值寄存器与运行期常量串注册表 |
| `runtime/opcode_profile.hpp` / `.cpp` | 可选指令频度探针 |

`AriaVM` 是不可复制、不可移动的宿主。主上下文和协程均为 `ObjMovement`，执行状态由
`AriaExeContext` 持有；宿主通过 `current_` 选择正在执行的上下文，恢复者通过 `previous_` 连接。
`AriaExeContext` 不继承 Object、无虚函数，仅允许经 ObjMovement 的 Object 虚析构销毁。

## 值栈、帧与 upvalue

- 值栈基于 `Buffer<Value>`，`top_` 指向下一空槽；扩容会搬迁，调用方不得跨 push 或重入持有栈地址。
- `FrameStack` 一次分配、永不扩容，元素须满足头文件中的 trivial 约束。`truncate(n)` 保留 n 个帧，
  值栈和 upvalue 的清理由上下文负责。
- `CallFrame` 缓存函数、CodeUnit、模块、ip 与局部槽基址。槽 0 是函数自身或方法接收者，兼作返回槽。
- `last_ip` 记录取指前的指令起点，用于错误定位和 try 记录查询；活动帧借用的字节码缓冲必须保持稳定。
  初始化及区间边界的原因见[异常笔记](../notes/runtime/exception.md) #1-#3。
- `enter_frame` 通过 `CallFrame::init` 进入 aria 函数帧；`exit_frame` 关闭本帧 upvalue 后退帧、截栈。
  `truncate_frames_to` 批量清理被弃帧，`reset` 清空执行状态但保留容量，不修改 `previous_` / `state_`。
- 开 upvalue 按槽址降序排列，同槽捕获复用同一节点；`close_upvalues(from)` 关闭槽址 >= from 的前缀。
  `ObjMovement::trace` 单独标记开链，防止仅被链引用的节点被回收。
- `grow_stack_` 在搬迁前保存 `top_`、活动帧 slots、upvalue location 的偏移，搬迁后重建。
  不得从已失效指针计算偏移；原因见[VM 笔记](../notes/runtime/vm.md)「硬红线」。

容量、深度上限与完整访问器以 `AriaExeContext.hpp` 为准。

## ctx 服务面

原生函数和 Object 协议通过 `AriaExeContext&` 访问分配器、宿主常量串和 bootstrap 类。
`host_` 是借用回指针，不参与 GC 追踪；bootstrap 尚未就位时不得调用依赖宿主服务的接口。

- `raise(Value)` 挂起原始载荷；`raise(code, ...)` 构造 ObjException 后挂起。寄存器非空时不得重复 raise。
- `fail` 挂起错误并返回 `FailSignal`，惯用出口为 `return ctx.fail(...)`。
  `FailSignal` 不适用于可能经 bool 构造出有值状态的 `Opt<标量>`，限制见头文件。
- `arity_error` 家族统一构造元数错误；`check_arity` 检查固定、默认与变参范围，
  `prepare_call_args` 补缺省印章、打包 rest 列表，供函数调用与协程首启共用。
- `str<key>()` 在编译期查询常量串下标，`str(index)` 和类型访问器在 `.cpp` 委托宿主。
  这使 Object 协议和原生方法无需包含完整的 AriaVM 定义。

## Eval 驱动与调用

`Eval` 通过友元访问宿主，除需跨协程搜索的 `unwind(AriaVM&)` 外，执行方法均面向上下文。
`dispatch_loop` 成功时结果在栈顶，失败时错误仍挂起。最外层由 run 处理，嵌套层交调用方续报。

循环内可能换指或弹帧的站点须跳到 `sync_now` 重取 current/frame；错误站点跳到
`unwind_check`。switch 之后和标签体内不得使用可能失效的 frame。定序依据见
[异常笔记](../notes/runtime/exception.md) #11。

### 函数与方法调用

调用区为 `[slot0, a1..aN]`。普通函数的 slot0 为函数值，方法的 slot0 为接收者。

- `call_value` 分派函数、类和绑定方法，其余对象通过 `load_field_from_class` 取 `__call__`；
  该分支先将 slot0 换为实际被调用对象。成员查询与方法绑定语义统一见 [object.md](object.md)。
- `call_aria_fn` 临时根化已解析函数，跨元数检查与实参整形保活；进入帧后由 `frame.function` 接管。
  方法可能已从类表移除，槽 0 也可能已被接收者覆盖，不能依赖它们继续持有函数。
- `call_class` 在分配实例前将类写入 slot0 保活；实例建成后接手该槽，并成为 init 的接收者。
- `call_native_fn` 同步执行、不进字节码帧。成功时上下文可能已切换，drop 仍落在进场 ctx；
  失败时必须已挂起错误，且不得切换。返回前不再读取已调用的原生函数对象。
- `PREPARE_METHOD` 在实参求值前通过裸查询解析目标并留栈保活；`CALL_METHOD` 去除目标槽，
  将 `[receiver, target, args...]` 整理为 `[receiver, args...]` 后分派。super 调用走绑定读取。
- `CLOSURE` 创建的函数须先压栈，再逐项捕获 upvalue；`CLOSE_UPVALUE` 只关闭不弹栈，
  与编译器发射的 POP_N 配对。安全依据见[异常笔记](../notes/runtime/exception.md) #19。

`NativeCallInfo` 用槽下标定位调用区，每次访问重取基址，允许原生执行期间重入并搬迁值栈。
实参只读，返回值通过 `set_return_val` 写槽 0；写入后 self 已被覆盖。
原生方法的接收者类型由 `receiver<T>()` 检查，注册方式见 [object.md](object.md#内置类型的成员面)。

### 嵌套执行边界（重入口）

`execute_function` / `execute_method` 共用 `Eval::execute_function`：原生载荷同步调用，
aria 载荷重入 dispatch_loop。aria 重入仅能发生在正在执行的驱动内。

`ret_frame_` 是当前驱动的返回边界，`recursion_depth_` 记录嵌套深度。
`enter_recursion` 保存外层边界，`leave_recursion` 恢复；失败时 unwind 清理边界内残帧，错误留给外层。
重入实参可借用当前值栈。压入调用区会触发扩容时，先复制实参；重入后须重新定位返回槽。
嵌套执行允许 resume，禁止 yield，原因见[协程笔记](../notes/runtime/coroutine.md) #10。

### 整数运算

Int 的统一值域在 `aria.hpp` 定义。算术结果越界经 IntegerOverflow 进入 VM 异常通道；
乘法须在计算前检查，避免宿主 i64 中间值溢出。除法向零截断，MIN / -1 溢出，MIN % -1 得零；
除模零保留 DivisionByZero。复合赋值或自增失败时不写回，已发生的求值副作用保留。
位运算的计数及结果边界、浮点规则见[指令集规格](specs/instruction-set.md)。

### 显示渲染与外部调用

对象显示由 `__str__` 定制，调试渲染 `value_repr` / `repr` 不执行用户代码。

| 入口 | 契约 |
| --- | --- |
| `Eval::stringify` | 非对象直接渲染；对象从所属类链取 __str__ 并同步调用，返回值须为 String；失败返回 nullptr 并挂起错误 |
| `Eval::concat_values` | 将各段经 stringify 渲染并临时根化，再定容拼接；输入元素由调用方保活，values 不得借用值栈 |
| `run_build_string` | 在值栈原槽逐段换装为串；每次访问重取地址，跨渲染重入保活并避开搬迁 |
| `execute_callable` | 私有钩子调用口，仅接受函数或绑定方法；由 execute_str_hook 验收显示结果 |

stringify 返回的串本身没有临时根，调用方须在下个 GC 点前发布或根化。
concat_values 可以借用容器缓冲，但渲染中用户代码修改该容器不设防；List 的 join 直接传入元素视图。

## AriaVM 宿主

### 执行入口

- `run(ObjFnProto*)` 执行良构的无参入口原型，返回 `Result<Value, Error>`。入口与出口 reset 清场，
  入口锚定主上下文，出口断言已回到同一上下文；最外执行不经重入边界。
- `run(SourceFile&, ObjModule*)` 编译并执行；source 须覆盖编译期，module 由编译入口根化。
- `interpret_from_src` / `interpret_from_path` 渲染错误到 stderr，返回 InterpretResult 类别。
  共用的 interpret_run 按失败阶段区分编译和运行错误；IMPORT 中被导入模块的编译失败属于运行阶段。
- 运行期未捕获错误仅在 run 的失败出口物化；Error 自有消息与位置，源文件销毁后仍可使用。

### 共享状态

| 状态 | 用途 |
| --- | --- |
| `modules_` | 规范路径 intern 串到模块对象的映射 |
| `builtins_` | VM 共享的只读内建表；LOAD_GLOBAL 在模块 globals 缺席时回退，STORE_GLOBAL 不回退 |
| `registers_` | bootstrap 类、缺参印章与 match 兜底异常；成员与格位由 value_register.hpp 派生 |
| `str_table_` | VM 自己按名取用的常量串；成员由 str_table.hpp 定义，源码中的普通名称由常量池保活 |
| `char_cache_` | 按字节值索引的驻留单字节串缓存 |
| `source_roots_` | [0] 为入口目录，[1..] 为配置根；set_source_roots 仅替换配置根，CLI 暂无配置口 |

宿主 tracer 标记 modules、builtins、registers、str_table、char_cache 与 current。
`ObjMovement::trace` 标记值栈、活动帧的函数与模块、开 upvalue 链、挂起错误，再沿 previous 链追踪。
常量串和字符缓存也须显式标根，驻留池不保活对象。GC 自动触发点见 [memory.md](memory.md)。

### 类与对象 bootstrap

构造期先注册宿主 tracer，在 GC lock 内完成常量串、类寄存器和内建注册，解锁前全部发布进根。
类注册以 `XxxClass::make_class` 为入口，Object 是父类为空的根，其余 bootstrap 类以它为父类。
实例方法与静态原语的绑定区别见 [object.md](object.md#内置类型的成员面)，类与格位以注册表为准。

### IMPORT 与模块加载

`resolve_module` 负责路径查找，`load_module` 只加载、编译并入表，模块体由 IMPORT 进帧执行，
不递归 dispatch_loop；模块入口返回模块对象，使命中与未命中的栈结果一致。
编译成功后先入表再运行模块体，循环导入复用半初始化对象，模块体失败不回滚表项。
路径串由 run_import 临时根化；加载失败返回 nullptr 并挂起错误。完整链路见 [import 规格](specs/import.md)。

### 协程

CoroutineClass 提供 create/resume/yield/status 静态原语，不绑定接收者；句柄为 ObjMovement。
create 只接受 aria 函数。首启 resume 的载荷作函数实参，后续 resume 只接受零或一个载荷。
主上下文不能 yield，嵌套执行限制见上文；挂起协程的 previous 必须为空。

| 动作 | 当前上下文 | 恢复者或目标 |
| --- | --- | --- |
| resume | 恢复者变 Normal | 目标由 Suspended 变 Running |
| yield | 变 Suspended 并断链 | 恢复者变 Running |
| 最外帧返回 | reset 后变 Done 并断链 | 恢复者变 Running，接收完成值 |
| 错误未捕获并转投 | reset 后变 Failed 并断链 | 恢复者变 Running，继续搜索 handler |

切换集中在 enter_coroutine / leave_coroutine。call_native_fn 的 drop 使调用区槽 0 留作预留结果槽，
对侧将 yield 或完成值写入该槽。首启需单独进帧。置链、检查与错误转投的顺序依据见
[协程笔记](../notes/runtime/coroutine.md) #1-#5；本文件不重复推导。
主上下文参与切换，但其 state 没有语言查询路径，不用于判定。

## VM 异常通道

VM 检测的错误装箱为 ObjException，用户 throw 保留原值，均通过 pending_error 传播。
`Eval::unwind` 用各帧 last_ip 查询 CodeUnit 的 try_records，在当前返回边界内由内向外搜索。

| 搜索结果 | 处理 |
| --- | --- |
| 命中 handler | unwind_to_handler 弃内层帧、关 upvalue、截栈并转入 catch，将错误值直接填入参数槽 |
| 嵌套边界内未命中 | 截净本轮嵌套帧，返回失败供外层继续搜索 |
| 零边界未命中且有恢复者 | 将当前协程置 Failed，转投错误并沿恢复者链继续搜索 |
| 链根仍未命中 | 保留帧链与错误，交 run 的失败出口物化 |

搜索阶段不逐帧弹栈；搜索后的派发和边界清理会改变帧栈或上下文。
错误只在站点 raise，unwind 由循环统一驱动，不合并为错误构造助手。关键定序见
[异常笔记](../notes/runtime/exception.md)与[协程笔记](../notes/runtime/coroutine.md)。

### 未捕获堆栈跟踪

`rebake_uncaught_error` 从存活帧按内到外生成 at 行，提取挂起值的码与消息，reset 后以
`Error::from_baked` 物化。ObjException 保留原码，其他抛出值使用 UncaughtException。
运行期消息本身不带位置；透传的编译错误保留自身已烘焙的位置。

死协程与失败重入的帧已在转投或边界清理时退场，不进入最终跟踪；物化不分配 GC 对象。
finally 和 defer 不参与异常流程，相关取舍见[异常笔记](../notes/runtime/exception.md)末节。

## 调试与画像

- `ARIA_DEBUG_TRACE_EXECUTION` 控制每条指令前的 trace，使用 Disassembler 输出指令和纯 C++
  值栈渲染，禁止通过 stringify 执行用户显示钩子。
- `ARIA_OPCODE_PROFILE` 经 `ARIA_FETCH_OPCODE` 在取指点采集计数、操作数和派发对；
  配对分类见 opcode_profile.cpp，prev_op 只存值，不借用可能在 REPL 行间失效的字节码地址。
- `ARIA_OPCODE_STATS` 控制析构时向 stderr 导出画像；驱动与格式见 [bench/profile](../../bench/profile/README.md)。
  静态发射计数另需 ARIA_DEBUG_PRINT_CODE 构建。
