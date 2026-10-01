# M3 异常（try/catch/throw）实现坑点记录

> M3 异常通道（`throw/catch` + VM 运行时错误统一走自管 unwind）的坑点归档，**异常类特性重启前必读**。设计基线见 `vm-design.md` §4.5-§4.8，机制现状见 `.claude/rules/runtime.md`「VM 异常通道」，发射侧见 `.claude/rules/compile.md` 的 try/catch/throw lowering 节。坑 #1-#16 属 M3、#17-#20 属 M4 闭包的 upvalue 关闭与截栈/弹帧交互（见下「M4 补录」节）；**坑编号被 `src/` 与 `tests/` 广泛引用，不得重排** -- 故 #9、#12 两节删除后编号留空不复用（#9 是「`UPtr` 要取 `.get()`」的编译期 gotcha，编译器即暴露、非承重；#12 的 THROW 单行语义已被坑 #7 表格覆盖）。范围：只做 try/catch/throw（`finally` 已裁撤，见下 M3b 节）。

---

## M3b finally 裁撤记录（2026-09）

finally 子句在定稿控制流语义后整体裁撤、不再进入路线图（`finally` 回归普通标识符；try/catch 保持原形态，try 须有 catch）。裁撤理由：其存在价值是资源善后，而语言层尚无制造 OS 资源的内建，参照实现 Lua/Wren 亦无 finally；单向门 -- 将来重新引入是向后兼容的语法增补。

- **善后后继：defer**（可选后续、不绑定里程碑）：注册善后表达式，函数正常退出与异常 unwind 时 LIFO 执行。defer 的善后体是函数/表达式，return/break/continue 天然绑定其内部，finally 特有的「控制流不得离开」问题在 defer 下消解；细节（求值时机 / 捕获语义 / 是否要 errdefer）重启设计时定。
- **两机制共享的 VM 难点（defer 落地时须面对，保留备忘）**：unwind 途中执行用户代码 + 善后代码自身抛错替换在途异常（原异常丢失）；且在途状态不能只住 `pending_error_`（unwind 途中执行嵌套 try/catch 时，catch 命中会清寄存器吞掉在途异常），须另存专用槽位后续传（坑 #7/#14 单寄存器模型的边界）。
- **曾定稿的 finally 控制流语义存档**（若将来重新引入 finally，按此起步）：finally 必达（正常结束 / catch 命中 / 异常续传三退出路径各执行一次）；finally 体内 return 一律禁止（嵌套 fun/lambda 的 return 绑定自身函数不受限）、break/continue 指向 finally 外循环禁止（体内自含循环允许）--编译期语义错误；throw 放行但替换挂起中的异常。「不继承 Java/Python/JS 覆盖语义」的既定取向（公认 footgun、生态以 lint 劝阻、与 aria 无静默错误行为取向冲突）复用于 defer 的善后体设计。

---

## 坑 #1：ip offset 语义 -- 指令起始 vs 指令结束后

**现象**：`find_try_handler(ip)` 用 `[begin, end)` 半开区间匹配。当 THROW 是 try 体的**最后一条**指令时，throw 发生时 `ip` 已被 `read_u8` 推过 THROW 操作码，指向下一条 = `end`（半开区间不含 end）-> 查不到 handler -> 本应被捕获的 throw 变成未捕获。运行时错误（如 `1/0`）发生在 try 体最后一条指令时同理。

**根因**：raise 时刻的 `frame.ip` 是**指令结束后**（已读入 opcode + 操作数），而 `try_records` 的 `end` 也是「try 体最后一条指令的下一条偏移」。两者都是「指令结束后」语义，但半开区间 `[begin, end)` 的 `end` 是 exclusive，而 raise 时 `ip == end` 落在区间外。

**对策**：raise/unwind 时用**指令起始**偏移查表，而非 `frame.ip`（指令结束后）。在 `dispatch_loop()` 主循环顶（`*frame.ip++` 之前）每轮把当前指令起始指针写进 `frame.last_ip`：

```cpp
while (true) {
    frame.last_ip = frame.ip;  // 每轮取指前记本指令起始(查表时与 unit->code.data() 相减反推 offset)
    switch (const OpCode op = *frame.ip++; ...) { ... }
}
```

raise 时用 `frame.last_ip`（指令起始）反推 offset 调 `find_try_handler`，不用 `frame.ip`。这样 THROW 作为 try 体末条指令时，`frame.last_ip` == THROW 起始 < `end`，命中。`frame.last_ip` 在 raise 发生前已被本轮循环顶写为本指令起始，故顶帧与外层帧可统一读之（见坑 #2），**无需 `ip_start` 寄存器快照、无需 raise 时另行计算偏移**。

---

## 坑 #2：跨帧传播需要 call-site 偏移，不是返回地址

**现象**：被调用函数内 throw，调用者 try 应捕获。但 unwind 遍历到调用者帧时，`frame.ip` 已被 CALL 推过（`CALL` + `argc` 操作数），指向调用者帧的下一条指令，而非 CALL 站点。用 `frame.ip` 算偏移查调用者 `try_records` 会偏移到 CALL 之后的位置，可能漏掉覆盖 CALL 站点的 try 记录。

**根因**：CALL 进帧后，调用者帧的 `ip` 停在「CALL 之后的下一条」，等被调用者 RETURN 后回来继续。异常 unwind 时要查的是「触发本次调用的 CALL 站点」是否落在某 try 区间内，而 CALL 站点 = `frame.ip - sizeof(CALL指令)`，不是 `frame.ip`。

**对策**：每帧记录「最后执行指令起始指针」。在 `CallFrame` 加 `u8* last_ip` 字段（无 NSDMI，`init_frame_` 置 code 起始，见坑 #3）。**在 `dispatch_loop()` 循环顶每轮取指前无条件写**（单次指针 store，热路径开销可接受，换取对所有 raise 站点的统一正确性；写入点代码同坑 #1）。**表示决策**：帧内存**指针**（与 `ip` 同为非 const 指向，运行期从不经二者写字节）、查表时与 `unit->code.data()` 相减反推 offset（`try_records`/行号表保持 offset 键不动）-- 与 `ip` 同取向（vm-design §4.3「裸指针走热路径、offset 冷路径换算」），主循环省一次基址减法与截断 cast；成立前提是帧存活期间 `code` 缓冲恒定（非移动 GC、CodeUnit 为 `ObjFunction` 值成员、执行期零 emit、IMPORT 现场编译新建 unit）。

每轮写一次覆盖全部 raise 情形，**无需分挂起点/raise 点、无需 raise helper 收 `ip_off`、无需 `ip_start` 快照**：

- **顶帧 raise**（THROW + 全部运行时错误：除零/类型不符/未定义全局/native fail/调用元数与溢出/IMPORT 等）：`frame.last_ip` 已被本轮循环顶写为故障指令起始，raise 时直接读 `frame.last_ip` 反推 offset。
- **外层帧（挂起于 CALL/IMPORT）**：CALL 被处理时，本轮循环顶已把 `frame.last_ip` 写为 CALL 站点起始；`enter_frame` 后调用者帧挂起，`last_ip` 停在 CALL 站点不变。unwind 到调用者读 `frame.last_ip`（= CALL 站点，反推 offset），查其 `try_records` 命中覆盖该 CALL 的 try。IMPORT 模块体调用同理。

**`unwind()` 统一读 `frame.last_ip`**（顶帧由本轮循环顶写、外层帧由其挂起前最后一轮循环顶写），不收参数、不区分顶帧/外帧。实现分两步（`AriaVM::unwind`，自最内帧向外遍历帧栈索引）：

1. **纯搜索，不动帧栈/值栈**：每帧以 `last_ip - unit->code.data()` 反推 offset 查 `find_try_handler`；未命中只往跟踪表 `push_back` 一个 `(function, module, ip_off)` 三元组（供未捕获时烘跟踪，坑 #16）。
2. **命中即就地派发**：`current_->unwind_to_handler(命中帧索引, record)` 一体完成弃帧/关开指/截栈/跳 handler/载荷落槽（见坑 #13），返 `nullopt`；全帧未命中则拆载荷 `(码, 消息)` + `reset()` 清场 + 逐帧烘焙 `at` 行，`Error::from_baked` 一次物化。

写收口在唯一一处（主循环取指前），自然覆盖所有指令、所有 raise 站点。故各报错站点只需就地 `raise`/`fail` 装箱后调 `unwind()`，**无需分挂起点/raise 点、无需专设 `*_and_unwind_` 融合助手**（早期曾议的 `raise_and_unwind_`/`throw_and_unwind_` 形态未采用，raise 与 unwind 是两个直观动作、站点就地两步）。

---

## 坑 #3：CallFrame 不能有默认成员初始化（NSDMI）-- 破坏 trivially-copyable

**现象**：给 `CallFrame` 加 `u8* last_ip = nullptr;`（默认成员初始化）后，`FrameStack<CallFrame>` 的 `static_assert(is_trivial_v<CallFrame>)` 失败。

**根因**：`FrameStack` 要求 T trivial + trivially-copyable + trivially-destructible（`static_assert` 三连）。NSDMI 会使类的默认构造函数变为非 trivial（即便初始化值是常量），导致 `is_trivial_v` 为 false。`CallFrame` 是 trivially-copyable 聚合，禁用 NSDMI。

**对策**：`CallFrame` 新字段**不写默认值**，初始化收敛进 `init_frame_`（Movement.cpp 已有此模式：`f.slots = ...; f.function = ...;` 逐字段赋值）。新增 `f.last_ip = f.ip;`（= code 起始；不用 nullptr -- 与 data() 相减是 UB）即可。保持 `CallFrame` 为纯聚合（无 NSDMI、无用户构造函数）。

```cpp
struct CallFrame {          // M3 时代快照:callable 字段当时为 ObjFunction*,M4 收敛后为 ObjClosure* closure
    ObjFunction* function;  // (现字段名与类型见 runtime/ObjMovement.hpp);坑本体(NSDMI 禁令)不受影响
    CodeUnit*    unit;
    ObjModule*   module;
    u8*          ip;
    Value*       slots;
    u8*          last_ip;   // 无默认值！init_frame_ 里置 = ip(code 起始)
};
```

---

## 坑 #4：try_records 须按 begin 升序 -- 嵌套 try 会推成降序

**现象**：嵌套 try（内层 try 在外层 try 体内）时，`find_try_handler` 二分查表找不到内层 handler。

**根因**：CodeGen 单遍编译，try 记录在 `visitTryStmtNode` 结束时 `push`。嵌套顺序：
1. 外层 try 开始：`begin_outer` 已知。
2. 编译外层体（含内层 try）：内层 try 开始 `begin_inner`（> `begin_outer`）-> 编译内层体 -> `end_inner` -> **push 内层记录**。
3. 外层体结束 -> `end_outer` -> **push 外层记录**。

push 顺序 = [内层(`begin_inner`), 外层(`begin_outer`)]，`begin_inner > begin_outer` -> **降序**。`find_try_handler` 二分假定升序 -> 失败。

**对策（定稿）：入口预插占位 + 结尾回填，构造即升序，不排序。**

编译末尾 `std::sort` 可行但多一遍 O(n log n)。更优：在 `visitTryStmtNode` **入口**即预插一条占位 `TryRecord`（`begin` 已知 = 当前 `cur_cu()->size()`，`end/handle/stack_depth` 先置 0/占位），拿到其索引 `rec_idx`；编译完 try/catch 体内层记录已按序插在其后（内层 `begin` > 本层 `begin`），最后**回填**占位项的 `end/handle/stack_depth`。

```
visitTryStmtNode:
  const u32 begin = cur_cu()->size();
  const usize rec_idx = cur_cu()->try_records.size();
  cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});  // 预插占位(begin 已定)
  begin_scope();                       // try 体 scope
  emit_stmt(node->body.get());         // 嵌套 try 在此编译,各自预插占位(begin > 本层 begin) -> 升序
  end_scope(line);
  const u32 end = cur_cu()->size();
  const usize jskip = cur_cu()->emit_jump(OpCode::JUMP, line);
  const u32 handle = cur_cu()->size();
  begin_scope();
  const u16 catch_slot = define_local_or_fail(*node->catch_param, node->loc());
  emit_stmt(node->catch_body.get());
  end_scope(line);
  patch_jump_or_fail(jskip, node->loc());
  // 回填占位项(编译末尾不再 sort):
  cur_cu()->try_records[rec_idx].end         = end;
  cur_cu()->try_records[rec_idx].handle      = handle;
  cur_cu()->try_records[rec_idx].stack_depth = stack_depth;  // try 入口快照
```

嵌套时内层 `visitTryStmtNode` 先于外层回填执行（内层整段在外层体内编译完），其预插占位 `begin_inner` > 外层占位 `begin_outer`，落在外层项之后 -> **整体按 begin 升序**，二分直接可用。`stack_depth` 在入口快照（try 体 `begin_scope` 前 `cur_fn_ctx()->locals_.size()`），与占位预插同处入口取值。

`find_try_handler` 的二分逻辑须返回**最内层**（最大 `begin` 且 `ip` 落 `[begin, end)` 的区间）--升序下即最后一个 `begin <= ip` 且 `ip < end` 的匹配项。

**核对（已读 `CodeUnit.cpp:141` 现有实现）**：当前二分**已是最内层语义**--先二分求 `low = 首个 begin > ip`，再从 `low-1`（begin 最大的 begin<=ip 项）向前找第一个 `end > ip`，即 begin 最大的覆盖区间，等价于「最内层」。**二分逻辑无需改动**。唯一改动：返回类型 `Opt<u32>`（仅 handle）→ `Opt<const TryRecord*>`（暴露 `stack_depth` 给 unwind，B1 已列）。前提「记录按 begin 升序」当前仅是注释假设（CodeGen 尚未发任何 try 记录），须由 B5 预插方案落实并实测嵌套 try 升序。

---

## 坑 #5：frame_depth 是运行时量，不能存进 TryRecord

**约束**：`TryRecord` 不得存 `frame_depth`（unwind 时刻的调用栈深度，编译期不可定）；帧深度由「从最内帧向外遍历帧链、首命中即在该帧 unwind」隐式决定，只存编译期可知的 `stack_depth`。字段定稿见 `vm-design.md` §4.5，`catch_slot` 恒等于 `stack_depth` 见坑 #10。

---

## 坑 #6：stack_depth 相对 frame.slots，不是栈基址

**现象**：unwind 截断值栈时用错基准 -> 截断到错误位置，破坏调用者帧的局部区。

**根因**：`stack_depth` 是「try 入作用域时**本帧**已声明的局部数」，相对 `frame.slots`（本帧槽 0），不是相对 `stack_base`（全局值栈基址）。跨帧 unwind 到 handler 帧时，要截断到 `handler_frame.slots + stack_depth`，丢弃 try 体内的临时值与本帧残留，但保留 handler 帧的 callee/参数/已声明局部。

**对策**：截栈基准按上式算，动作住 `Movement::unwind_to_handler(命中帧索引, record)`（一体完成弃帧 / 关开指 / 截栈 / 跳 handler / 载荷落槽，见坑 #13/#17）：先按槽址关开指（槽区仍存活），再截到 `handler_frame.slots + stack_depth`，最后 `push(载荷)` 恰落 catch 参数槽（值填槽，无需 `STORE_LOCAL`，见坑 #10）。早期外露的 `Movement::truncate_stack(usize)` 已删除、职责并入该执行体。

---

## 坑 #7：单寄存器模型 -- pending_error_ : Opt<Value> + ObjException 包运行时错误

**动机**：抛出的实体有两个侧面--catch 侧面要绑 aria 值保留类型（`throw 42` → e=Int 42）；run() 侧面未捕获要回结构化 `Error`（保 `ErrorCode` + 位置串，供 `test_ariavm` 9 处 `error().code()` 断言）。单一 `Opt<Error>` 会丢类型（throw 时刻值就压成串）；单一 `Opt<Value>` 会丢码（未捕获恒 `UncaughtException`）。解法：单寄存器 `Opt<Value>`，**运行时错误包成 `ObjException`（携码）**、**用户 throw 存原值（携类型）**，两种载荷同住一个 Value 寄存器。

**ObjException**（新 Object 子类型，`ObjType::EXCEPTION` 枚举早已预留）：
```cpp
class ObjException final : Object {
    ErrorCode  code_;     // 错误码
    ObjString* message_;   // 完整烘焙消息(与 Error::message() 同形;运行期装箱**不含位置前缀**(2026-09-10 改定,位置归未捕获出口的 at 跟踪行),编译期透传的错误为 path:line:col:,见坑 #7/#15)
public:
    ErrorCode  code()    const noexcept { return code_; }
    ObjString* message() const noexcept { return message_; }   // catch 的 println(e)/str(e) 渲染此串
    void trace(GC& g) const noexcept override { mark_obj(g, message_); }   // 标 message_ GC 串
    String to_string() const override { return message_->view(); }        // println(e) -> 消息
};
```
工厂 `new_exception(gc, code, StringView message)`（工厂内部 `new_string` 驻留并自守，见 `ObjException.hpp`）。`message_` 存**完整烘焙消息**（运行期不含位置前缀，位置由未捕获出口的 `at` 跟踪行给出；透传的编译期错误自带位置，原样保留）--catch 绑 ObjException 即绑原码原消息，位置不丢、catch UX 不退化。

**单寄存器 `pending_error_ : Opt<Value>`**（替代 `Opt<Error> + Opt<Value>` 双寄存器）：

| 路径 | 写入 pending_error_ | catch 绑定 | 未捕获 run() 回传 |
|---|---|---|---|
| 用户 `throw V` | `V`（原值，不包） | `V`（保类型：`throw 42`→e=Int 42） | `Error::from_detail(ErrorCode::UncaughtException, format_value(V))` |
| 运行时错误 / native `fail` | `new_exception(gc, err.code(), err.message())`（包成 ObjException，工厂内部驻留） | ObjException（`println(e)` 渲染消息；re-throw 保码） | `Error::from_baked(ex.code(), ex.message()->view())`（原码 + 原消息） |

**Error 需「原样装载已烘串」的构造入口**（已落地为静态工厂 `from_baked(code, baked_string)`）：烘焙路径（`from_detail` 无位置/带位置两重载）都经 `make_message`（对已烘焙消息双重前缀）。`from_baked` 跳过 `make_message`、直接装已烘焙串--两类合法调用方是 ObjException 反提 `Error` 回传 run() 与 dispatch_loop 直报站点 `runtime_err`。收口于 Error.hpp（构造面见 `.claude/rules/error.md`）。

**两硬约束都满足**：
- 类型保留：用户 throw 存原值，catch 绑原值 ✅。
- 码保留：运行时错误包成 ObjException（携 `code_`），未捕获经 `from_baked` 回原码 ✅（`TypeMismatch`/`DivisionByZero`/`WrongArity`/`StackOverflow`/`UndefinedVariable`/`ModuleNotFound` 等 9 处断言过）。

**比双寄存器多的语义收益：re-throw 保码**。`try {1/0} catch(e) { throw e }` 未捕获：
- 双寄存器：catch 绑懒合成消息串 → `throw e` 存串 → 未捕获回 `UncaughtException`（**丢 DivisionByZero**）。
- 单寄存器：catch 绑 ObjException → `throw e` 存同一 ObjException → 未捕获 `from_baked` 回 `DivisionByZero` ✅。

**catch 绑 ObjException 的 M3 可用性**：`println(e)`/`str(e)` 渲染消息 ✅；字符串拼接需 `"x" + str(e)`（`e` 非字符串，`e + "x"` 类型错）；`e.message()`/`e.code()` 留待 M5 方法/字段落地。M3 catch-of-runtime-error 的字符串操作多一个 `str()` 调用，可接受。

**站点改动**：`call_*` 失败 / `call_native` / 各处报错站点一律置 `pending_error_ = new_exception(...)`（包一层 ObjException；raise 不烙位置前缀，位置归未捕获出口的 `at` 跟踪行，见坑 #15/#16）。`vm.fail(code, ...)` 助手内部包，原生函数与 call_* 失败站点调用点不变（报错装箱统一收口 `AriaVM::raise`/`fail`）。用户 `throw` 存原值不包。`unwind` 命中 handler 时 `push(*pending_error_)`（无需懒合成分支）。

---

## 坑 #8：pending_error_ 的 GC 安全 -- 不在值栈内，须由 tracer 显式标根

**现象**：`pending_error_ : Opt<Value>` 持 aria `Value`，运行时错误时装箱 `ObjException*`、用户 throw 时可能装箱任意 `Obj*`（如 `throw "msg"` 的 ObjString）。它存在 `Movement` 成员里，**不在值栈 `[base, top)` 区间内**，VM 根 tracer 默认只标值栈 + 帧的 function/module + modules_ + builtins_。raise 到 unwind 之间若触发 GC（如 `new_exception` 顶 `maybe_collect`、handler 处截栈 + `push` 顶栈增长触 `maybe_collect`），`pending_error_` 内的 Obj 无根可达 -> 被回收 -> catch 绑定到悬垂对象。

**对策**：`pending_error_` 必须纳入 VM 根 tracer。在 `AriaVM` 的 vm_roots tracer 里加（已落地，且为沿 current_ 执行链逐个标形态 -- 挂起协程的寄存器同样是根）：

```cpp
// 标挂起错误/抛出值(若持有 Obj，unwind 前可能跨 GC) -- 沿 current_ 执行链逐个标
for (Movement* m = current_; m != nullptr; m = m->previous()) {
    if (m->has_error()) {
        mark_value(gc, *m->pending_error());   // Value 可能是 ObjException* 或原 Obj*
    }
}
```

`mark_value` 对 ObjException 会进一步 `ObjException::trace` 标其 `message_` ObjString。**不可遗漏**，否则 stress GC 下 try/catch 路径 use-after-free。

**构造期守卫（已由工厂自守化解）**：`raise` 内 `new_exception(gc, code, message)` 的 message 参数是 C++ 侧 `String`（`Error::make_message` 产物，非 GC 对象），工厂内部 `new_string` 驻留并自行守卫跨下方 `new_object`（工厂守「自己创建的」，见 ObjException.hpp）；返回对象白色无根，但 `raise` 收尾即入 `pending_error_`（之间无分配），入寄存器后经 tracer 根化，无需调用方 `make_guard`。

**测试**：try/catch 路径必须开 stress GC（`gc.set_stress(true)`）验证根接线，复刻 `run_source` 已有的 stress 模式。

---

## 坑 #10：catch 参数绑定走「值填槽」-- 无 catch_slot 字段、无 STORE_LOCAL 前导

**现象（化简前旧设计）**：旧 lowering 在 `TryRecord` 存 `catch_slot` 字段、handler 首条发 `STORE_LOCAL catch_slot`。但推一遍栈位发现两者皆冗余。

**根因**：项目的「值填槽」不变式（compile.md：`下个局部 slot = 当前栈高`，初始化值 push 后恰好落在该槽）在异常路径上同样成立：
- `stack_depth` = try 入口（try 体 `begin_scope` **之前**）局部数 = `cur_fn_ctx()->locals_.size()` 快照。try 体局部在 try scope 内声明，unwind 时丢弃（截到 `frame.slots + stack_depth`）。
- catch 参数在 try 体 `end_scope` **之后**声明 -- try 体局部已弹，下一个可用槽即 `stack_depth`，故 `catch_slot == stack_depth`，恒等。存进 `TryRecord` 是冗余字段。
- unwind 截到 `frame.slots + stack_depth`（top = `slots + stack_depth`）后 `push(thrown_value)` -- 值落在 `slots + stack_depth` = **slot `stack_depth` = catch 参数槽**。`e` 已被绑定。
- handler 首条若再 `STORE_LOCAL catch_slot`：peek 栈顶（`slots + stack_depth`）写回 slot `stack_depth`（`slots + stack_depth`）-- **自赋值 no-op**。该指令冗余。

**对策**：
1. `TryRecord` 只存 4 字段 `{begin, end, handle, stack_depth}`，去掉 `catch_slot`。
2. unwind 在截栈后 `push(thrown_value)`，异常值自然落在 `e` 的槽（见坑 #6）--handler 不发 `STORE_LOCAL` 前导，直接进 catch 体。
3. `e` 须收进 **catch 子句 scope**（包住 `e` + catch 体），让 `end_scope` 连 `e` 一起弹，两条路径在 `L_end` 栈高齐平。旧模板把 `e` 声明在 catch 体 scope 之外（`e` 不被弹），靠 `STORE_LOCAL` no-op 遮住栈失衡隐患；去掉 `STORE_LOCAL` 后必须把 `e` 入 scope 才平衡。

**lowering 模板**（化简后，入口预插占位 + 结尾回填，见坑 #4）：
```
const u32 stack_depth = cur_fn_ctx()->locals_.size();   // try 入口局部数（try scope 开前）
const u32 begin = cur_cu()->size();
const usize rec_idx = cur_cu()->try_records.size();
cur_cu()->try_records.push(TryRecord{begin, 0, 0, 0});  // 入口预插占位(begin 已定,end/handle/stack_depth 待回填)
begin_scope();                       // try 体 scope
emit_stmt(node->body.get());         // 嵌套 try 在此编译,各自入口预插(begin > 本层) -> 整体升序(坑 #4)
end_scope(line);                     // 正常路径弹 try 体局部 -> 栈高 stack_depth
const u32 end = cur_cu()->size();
const usize jskip = cur_cu()->emit_jump(OpCode::JUMP, line);  // 正常跳过 catch
const u32 handle = cur_cu()->size();                         // L_catch(unwind 已 push 落 e 槽)
begin_scope();                       // catch 子句 scope(包 e + catch 体)
const u16 catch_slot = define_local_or_fail(*node->catch_param, node->loc());
emit_stmt(node->catch_body.get());
end_scope(line);                     // 弹 catch 体局部 + e -> 栈高 stack_depth
patch_jump_or_fail(jskip, node->loc());      // 正常路径 -> L_end(此处栈高 stack_depth)
cur_cu()->try_records[rec_idx].end         = end;         // 回填占位项
cur_cu()->try_records[rec_idx].handle      = handle;
cur_cu()->try_records[rec_idx].stack_depth = stack_depth;
```

**栈平衡校验**：正常路径 try 体 `end_scope` 弹至 `stack_depth`；异常路径 catch 子句 `end_scope` 弹至 `stack_depth`（`e` + catch 体局部）；两路径在 `L_end` 均栈高 `stack_depth`，齐平。

**注意**：`catch_slot` 在 lowering 里仍由 `define_local_or_fail` 拿到（catch 体经 `LOAD_LOCAL catch_slot` 读 `e`），但它 == `stack_depth`，**只编译期用、不入 `TryRecord`**。`e` 由 unwind 的 push 在运行期填槽）。

**核对（已读 `FunctionCtx.cpp` 现有实现；M4 起收口为单方法 `FunctionCtx::end_scope()`）**：`end_scope` **真正 `locals_.pop_back()` 移除**原 scope 局部（非只减 `scope_depth_`），故 `locals_.size()` 精确反映当前活局部、**无陈旧项堆积**。`stack_depth = cur_fn_ctx()->locals_.size()`（try 体 `begin_scope` 前快照）= try 入口活局部数 = 下一可用 slot = catch 参数槽。try 体 `end_scope` 弹回 `stack_depth`，catch 参数 `add_local` 后 `locals_.size() == stack_depth + 1`、catch 参数 slot == `stack_depth`。定稿成立，无问题。

---

## 坑 #11：dispatch_loop() 错误传播重构 -- 命中 handler 后必须回循环顶重取 frame 引用

**现象**：把 `return runtime_err(...)` 改为 raise+unwind 后，命中 handler 的路径若不 `continue` 而是继续 `break`/落到 switch 尾部，会触用已被 `frames_.truncate` 弹掉的旧帧引用 -> use-after-free。

**根因**：`dispatch_loop()` 主循环 `switch (CallFrame& frame = frames.top(); ...)`。`unwind()` 命中 handler 时 `frames_.truncate` 可能弹掉多帧，`frame` 引用悬垂。必须 `continue` 回循环顶重新取 `frames.top()`。

**对策**：统一模式（装箱与 unwind 是两个直观动作、就地两步）：
```cpp
raise(ErrorCode::X, "...");             // 报错站点就地装箱(原生站点经 vm.fail)
if (auto u = unwind()) {
    return runtime_err(std::move(*u));   // 未捕获 -> 终止 dispatch_loop
}
break;                                   // 命中 handler -> frame 已废,回循环顶重取
```

约 25 处 `return runtime_err(...)` 站点（算术/比较/NEGATE/LOAD|STORE_GLOBAL/CALL/IMPORT/THROW）改为此模式。**每处命中分支都不得再触用旧 `frame` 引用**，须回循环顶重取。

**终态（2026-09）**：派发站点与普通 case 统一以 `break` 退出（switch 即整个 while 体、其后无语句，与早期写法 `continue` 等效）；防御意图改由 dispatch_loop 循环顶注释钉住（switch 之后不得新增引用 `frame` 的代码）。

**终态补记（2026-09-24）**：26 处站点的三行检查样板收口为循环尾单一标签 `unwind_check`（switch 后 `continue` 隔离正常路径，错误站点 `goto` 跳入；未捕获 `return`、命中 handler 落回循环尾回循环顶重取帧）。switch 之后现有标签体，仍不引用 `frame`，坑前提不变。

例外（已消除，M3 前置改造）：IMPORT 内 `load_module` 已统一走寄存器--返 `ObjModule*`（`nullptr ⟺` 载荷已 raise 入 `*current_`），编译期 Error 就地 `new_exception` 原样装配箱（from_baked 语义不重烘，code+消息逐字节保真，位置语义见坑 #15/#16）、`ModuleNotFound` 经 `fail` 烘 IMPORT 站点位置，调用方 `take_error` 取出传播。M3 改造时它不再是直传残留。

`call_value` 失败站点：
```cpp
if (!call_value(callee, argc)) {                 // 作用于 *current_,失败载荷已 raise 入挂起寄存器
    if (auto u = unwind()) return runtime_err(std::move(*u));  // 未捕获 -> 终止 dispatch_loop
    break;                                       // 命中 handler -> frame 已废,回循环顶重取
}
```

---

## 坑 #13：unwind() 帧遍历（终态：纯搜索，不再逐帧 `exit_frame`）

**约束**：遍历必须**不动帧栈** -- 未命中的帧只记跟踪三元组（帧引用全程有效），命中后交由 `Movement::unwind_to_handler(命中帧索引, record)` 一体完成弃帧 + 关开指 + 截栈 + 跳 handler + 载荷落槽（见坑 #6），全帧未命中走 `reset()` 一次清场。

旧形态「未命中即 `ctx.exit_frame()` 逐帧弹」已废，两处硬伤：弹帧使循环内的帧引用悬垂（命中后继续用旧引用即 use-after-free）；走到「所有帧无 handler」时帧已全弹、跟踪无帧可查，故跟踪必须改为遍历中顺路收集（坑 #16）。机制现状见 `rules/runtime.md`「VM 异常通道」。

---

## 坑 #14：单寄存器 `pending_error_ : Opt<Value>` 的落地分工

**约束**：`Movement` 不持 `GC&`，故**装箱上移** -- `AriaVM::raise(code, detail)` / `fail` 负责构造 `ObjException` 并 `ctx.raise(Value)` 存入，`Movement` 只暴露 `raise(Value)`（存原值、不包）与 `has_error()` / `take_error()` / 只读 `pending_error()` 访问器；载荷类型为 `Opt<Value>`（装 ObjException 或用户 throw 原值，见坑 #7），`reset()` 一并清空。

旧形态的两件均已删除：`truncate_stack`（职责并入 `unwind_to_handler`，见坑 #13）与双寄存器 `pending_throw_` 四件套。`pending_error_` 的 GC 标根见坑 #8。

---

## 坑 #15：运行期位置标注（已裁撤：位置只归未捕获跟踪行）

**现行契约**：运行期错误的位置**不烘入** `ObjException` 消息，消息形态 = `Error::make_message(code, detail)`（无位置版）；位置唯一载体是未捕获出口的堆栈跟踪 `at` 行（坑 #16）。顶帧 `last_ip` 即故障指令、`call_*` 失败与原生报错即 caller 帧的 CALL 站点（原生不进帧），故无需在 take 点补标、无双重标注；`last_ip` 由主循环取指前写（坑 #1/#2），本坑为其又一消费方。透传的被导入模块编译期错误自带位置、不经 unwind 亦不标注。

早期形态「装箱点烘 `path:line` 前缀」及配套的 `runtime_loc` 助手、`raise_detail` 壳、`with_runtime_loc` 两段式均已删除（与跟踪行信息重复，且 THROW 原值与透传编译错两路本就无/自带位置）。完整语义见 `vm-design.md` §4.8 与 `rules/runtime.md`。

---

## 坑 #16：未捕获堆栈跟踪（unwind 顺路收集，物化时烘焙）

**约束**：跟踪在 uncaught 出口一次性生成、**不存 `ObjException`**（对标 Python：traceback 取自活帧，异常对象不背全程 trace；catch 掉的异常多数用不上）。落点 = `unwind()` 遍历帧链时**每帧退出前**顺带收集 `(function, module, last_ip)` 三元组--走到未捕获时帧已全弹，故必须顺路收集；命中 handler 则收集弃用。`last_ip` 在此两用（查表 + 跟踪行号）：顶帧 = 故障指令、外层帧 = CALL 站点。物化 `Error` 时按收集序（内 -> 外，最内帧紧贴消息行，主流 traceback 惯例）逐帧格式化为 `  at <fn名> (<位置串>)` 追加到 `Error::message_` 尾部--烘焙进 message 而非 VM 直接输出，`interpret_run` 打印零改动、测试可断言、嵌入方自行决定展示。透传错误无跟踪。语义与输出示例见 `vm-design.md` §4.8 与 `rules/runtime.md`「未捕获堆栈跟踪」。

---

## M4 补录：闭包 upvalue 关闭与 unwind 的交互坑点（2026-09，已落地）

> M4 闭包在 M3 的截栈/弹帧机制上叠加「upvalue 关闭」维度：局部槽还可能被 open upvalue 链上的 `ObjUpvalue` 开指着，故「丢弃一段栈区」的每条路径（RETURN 弹帧 / unwind 命中截栈 / 未命中弹帧 / 作用域退出）都必须先回答「区间内开指何时关、谁负责关」。四条记录的对策均已实施，测试钉在 `tests/runtime/test_ariavm.cpp`（闭包机制节）与 `tests/compile/test_codegen.cpp`（M4 节）。

## 坑 #17：unwind 命中分支的关闭点 -- close 必须先于截栈与 push，且 handler 帧不退、不经 exit_frame

**现象**：try 体内声明并被闭包捕获的局部（`try { var x = ...; ... } catch (e) {}`，x 的 upvalue 开指着本帧槽），若异常 unwind 命中 handler 时不关该区间开指，catch 及其后经闭包读 x 得脏值。

**根因**：unwind 的两条丢弃路径不对称，命中分支不能搭 RETURN 的便车：
- **未命中帧 / RETURN**：帧被 `exit_frame` 弹掉，关闭可内置其内（坑 #18）。
- **命中 handler**：**帧不退**（执行点跳回本帧 catch 入口，handler 帧不弹），不经 `exit_frame`；丢弃的只有值栈区间 `[slots + stack_depth, top)`（坑 #6 的截断基准）。try 体被捕获局部恰好住在该区间（try 体局部自 slot `stack_depth` 起声明，坑 #10），必须显式关闭。

且顺序**承重**：关开指 -> 截栈 -> `push(载荷)`（三步都住 `Movement::unwind_to_handler`）。载荷 push 落 catch 参数槽 == slot `stack_depth` == **弹弃区首槽**（坑 #10 的「值填槽」约定在此反咬一口）：若不先关，push 覆写被捕获局部槽，迁值读到的是异常值而非局部末值 -- 闭包此后读到的是 catch 参数。截栈本身只移 `top_` 不覆写，不承重；截栈后的**首个覆写点**就是 push，close 必须在它之前。

**对策**：命中分支三步定序，收口在 `Movement::unwind_to_handler`（坑 #13）：`close_upvalues(catch 槽)`（槽区存活时迁值，基准与坑 #6 截断同源）-> 截栈 -> `push(take_error())`。测试：`UnwindHitClosesTryBodyUpvalue`（命中：try 体 upvalue 随丢弃区间迁移，catch 覆写后读 5）/`UnwindClosesCapturedUpvalue`（未命中：经 `exit_frame` 关闭，catch 覆写陈旧槽后仍读 43）。

## 坑 #18：「帧退出 ⇒ 本帧区间开指全关」单点收口进 exit_frame + reset() 安全网

**对策（设计决策）**：RETURN 与 unwind 未命中两条路径都要「先关再弹」。与其让各调用方自己配对（易漏一条），不如把 `close_upvalues(frame.slots)` **内置进 `exit_frame`**，与弹帧、值栈顶复位一体 -- 「帧退出 ⇒ 本帧区间开指全关」成为单点结构保证，调用方无法只做其一而破坏值栈/帧栈/开链三者的对应关系。

**两个实现细节**：
- `exit_frame` 内**先取 `slots` 再 close/pop**：`frames_.top()` 引用在 pop 后悬垂，不可先 pop 再读（与坑 #2 同族的引用生命周期纪律）。
- 关闭须在**槽区仍存活**时（复位 `top_` 之前）完成：弹帧后调用者的后续 push 自 callee 槽起逐槽覆写已弃局部区，未迁值的被捕获局部会被踩掉（与坑 #17 的 push 覆写同因，方向相反：一个是 unwind 载荷，一个是 RETURN 后调用者的栈增长）。

**reset() 安全网**：HALT 收场**不弹帧**（`case HALT: return` 直接出 dispatch_loop；CodeGen 从不发射 HALT，它是手写字节码/嵌入方的逃生口），帧上开指残留在链上。`run()` 复用主上下文（REPL 逐行、测试多次 run），残留开指会跨 run 指入被覆写的栈区，再经幸存闭包（如挂在模块 globals 上的）读出脏值。故 `Movement::reset()` 先 `close_upvalues(buf_.data())`（全链）再清场 -- `run()` 前后各一次的清场即激活此安全网。

## 坑 #19：CLOSE_UPVALUE 批量关闭语义（Lua OP_CLOSE 式）与发射时序的安全性前提

**定夺**：作用域退出的显式关闭指令**不弹栈**（原计划为「关指顶槽并弹顶」`close_upvalues(top-1) + pop()`，实施中翻为）：`CLOSE_UPVALUE` 栈效应 `[] -> []`，语义对齐 Lua `OP_CLOSE` -- 关闭所有槽址 >= 当前栈顶的开 upvalue，弹栈全由前置 `POP_N` 承担。编译器发射：`POP_N`（整区一条，被捕获局部一并计数）-> 弹区含被捕获局部才追加一条 `CLOSE_UPVALUE`（发射点收口 `CodeGen::emit_pop_locals_to`，块/for/for-in per-iteration/break/continue/try 两 end_scope 共用）。

**安全性前提（坑点本体）**：POP_N 之后弹区槽已位于新栈顶**之上**，close 的迁值仍读它们，安全靠三条：
1. `POP_N` 与 `CLOSE_UPVALUE` 均无分配、无 GC 安全点，两指令间无任何覆写触发点；
2. close 只读弹区不写弹区（迁值写各自 `ObjUpvalue.closed_`）；
3. 外层帧槽址恒低于本帧（帧 slots 区间自底向上嵌套），故「槽址 >= 本帧新栈顶」的开 upvalue 只属弹区局部，不误关外层。

**若将来在作用域出口的 POP_N 与 CLOSE_UPVALUE 之间插入任何 push（调试钩子/defer 类机制），前提 1 即破** -- 这是 defer 落地时（若走本发射模式）须重新核对的一条。

**for-in per-iteration 出口**：每轮循环体结束都发 CLOSE_UPVALUE（若捕获），每轮关旧 upvalue、下一轮捕获全新一份 -- 「每轮新鲜绑定」的落地机制（测试 `ForInPerIterationCloseUpvalue`/`ForInNoCloseWithoutCapture` 钉位置与条件发射）。

## 坑 #20：open upvalue 链的 GC 标根与值栈增长重绑

**GC 悬垂（clox 已知坑）**：链上节点可能**仅被链本身引用** -- 闭包已死（不可达）而其 upvalue 仍在链上（要等作用域出口/帧退出才关）。GC 若不标开链，该节点被回收，链上留悬垂指针，后续 `close_upvalues`/`capture_upvalue` 走链即 use-after-free。故 vm_roots tracer 沿 `current_` 执行链逐 Movement 标开链各节点（「闭包已死而 upvalue 仍在链」防线；闭包可达时经其 trace 双标，mark 幂等无害）。测试：`OpenUpvalueChainSurvivesGcWithDeadClosure`（死闭包 + 开链节点跨 GC 存活）。

**值栈增长重绑（第三类指针）**：open 态 upvalue 的 `location_` 指入值栈，`grow_stack_` 搬运后成 dangling。与 `top_`/各帧 `slots` 同法**偏移两趟**：搬运前走链把各 `location_ - old_base` 记入 `List<usize>`（链节点数不定，不能走帧那样的栈内定长数组；链序两趟间稳定，免数节点一趟），搬运后 `new_base + 偏移`逐节点重建。对 dangling 指针做指针减法是 UB，故偏移必须**搬运前**记、两趟中不碰旧指针。测试：`StackGrowsRebasesOpenUpvalues`（2048 值压栈两轮 2x 增长后 upvalue 重绑仍读对）。
