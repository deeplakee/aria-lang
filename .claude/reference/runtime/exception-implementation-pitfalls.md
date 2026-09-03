# M3 异常（try/catch/throw）实现坑点记录

> 本文是 M3 异常通道（aria 语言 `throw/catch` + VM 运行时错误统一走 VM 自管 unwind）实现过程中踩到的坑的归档，供下一次实现参考。设计基线见 `vm-design.md` §4.7，落地状态见 `.claude/rules/runtime.md`「VM 异常通道落地状态」。一次实现尝试因下列坑叠加导致 5/11 测试失败而回退，本文记录根因与对策，避免重蹈。
>
> 范围：M3 只做 try/catch/throw（`finally` 推迟至子里程碑 M3b，不引入 `END_FINALLY`）。

## 设计基线速览（已定）

- **无 `SETUP_EXCEPT`/`END_EXCEPT` 指令**：`try` 的范围与 handler 地址由编译期写入 `CodeUnit::try_records`（异常记录表），运行时按 `ip` 查表。比操作码方案紧凑、不污染字节码流、便于反汇编。
- **不依赖 C++ 异常**：与 Lua/CPython 一致。`op` 返回失败 `Error` 后，`run()` 调 raise -> 查表 -> `FrameStack::truncate` 跨帧 unwind -> 跳 handler。
- **单寄存器模型**：`pending_error_ : Opt<Value>`（单一挂起寄存器，既装运行时错误包成的 `ObjException`，也装用户 `throw` 的原值）。运行时错误经 `new_exception(gc, code, new_string(baked_message))` 包成 `ObjException{code, ObjString* message}` 存入；用户 `throw V` 直接存 `V`。catch 绑 `pending_error_` 里的 Value（运行时错误绑 ObjException、用户 throw 绑原值，保类型）。未捕获时 `run()` 从 Value 反提 `Error`：ObjException 经 `Error::with_message(code, message)`（跳过 `make_message` 的重建工厂）回传原码+原消息（含位置）；原值回 `Error{UncaughtException, format_value(V)}`。详见坑 #7。
- **`TryRecord` 字段**：`{begin, end, handle, stack_depth}`（`frame_depth` 不存见坑 #5；`catch_slot` 不存见坑 #10 —— 恒等于 `stack_depth`，且 unwind 的 `push` 已把异常值落在该槽，无 `STORE_LOCAL`）。

---

## 坑 #1：ip offset 语义 -- 指令起始 vs 指令结束后

**现象**：`find_try_handler(ip)` 用 `[begin, end)` 半开区间匹配。当 THROW 是 try 体的**最后一条**指令时，throw 发生时 `ip` 已被 `read_u8` 推过 THROW 操作码，指向下一条 = `end`（半开区间不含 end）-> 查不到 handler -> 本应被捕获的 throw 变成未捕获。运行时错误（如 `1/0`）发生在 try 体最后一条指令时同理。

**根因**：raise 时刻的 `frame.ip` 是**指令结束后**（已读入 opcode + 操作数），而 `try_records` 的 `end` 也是「try 体最后一条指令的下一条偏移」。两者都是「指令结束后」语义，但半开区间 `[begin, end)` 的 `end` 是 exclusive，而 raise 时 `ip == end` 落在区间外。

**对策**：raise/unwind 时用**指令起始**偏移查表，而非 `frame.ip`（指令结束后）。在 `run_()` 主循环顶（`*frame.ip++` 之前）每轮把当前指令起始偏移写进 `frame.last_off`：

```cpp
while (true) {
    frame.last_off = static_cast<u32>(frame.ip - frame.unit->code.data());  // 每轮取指前记本指令起始
    switch (const OpCode op = *frame.ip++; ...) { ... }
}
```

raise 时用 `frame.last_off`（指令起始）调 `find_try_handler`，不用 `frame.ip`。这样 THROW 作为 try 体末条指令时，`frame.last_off` == THROW 偏移 < `end`，命中。`frame.last_off` 在 raise 发生前已被本轮循环顶写为本指令起始，故顶帧与外层帧可统一读之（见坑 #2），**无需 `ip_start` 寄存器快照、无需 raise 时另行计算偏移**。

---

## 坑 #2：跨帧传播需要 call-site 偏移，不是返回地址

**现象**：被调用函数内 throw，调用者 try 应捕获。但 unwind 遍历到调用者帧时，`frame.ip` 已被 CALL 推过（`CALL` + `argc` 操作数），指向调用者帧的下一条指令，而非 CALL 站点。用 `frame.ip` 算偏移查调用者 `try_records` 会偏移到 CALL 之后的位置，可能漏掉覆盖 CALL 站点的 try 记录。

**根因**：CALL 进帧后，调用者帧的 `ip` 停在「CALL 之后的下一条」，等被调用者 RETURN 后回来继续。异常 unwind 时要查的是「触发本次调用的 CALL 站点」是否落在某 try 区间内，而 CALL 站点 = `frame.ip - sizeof(CALL指令)`，不是 `frame.ip`。

**对策**：每帧记录「最后执行指令起始偏移」。在 `CallFrame` 加 `u32 last_off` 字段（无 NSDMI，`init_frame_` 置 0，见坑 #3）。**在 `run_()` 循环顶每轮取指前无条件写**（一次 u32 store，热路径开销可接受，换取对所有 raise 站点的统一正确性）：

```cpp
while (true) {
    frame.last_off = static_cast<u32>(frame.ip - frame.unit->code.data());
    switch (const OpCode op = *frame.ip++; ...) { ... }
}
```

每轮写一次覆盖全部 raise 情形，**无需分挂起点/raise 点、无需 raise helper 收 `ip_off`、无需 `ip_start` 快照**：

- **顶帧 raise**（THROW + 全部运行时错误：除零/类型不符/未定义全局/native fail/调用元数与溢出/IMPORT 等）：`frame.last_off` 已被本轮循环顶写为故障指令起始，raise 时直接读 `frame.last_off`。
- **外层帧（挂起于 CALL/IMPORT）**：CALL 被处理时，本轮循环顶已把 `frame.last_off` 写为 CALL 站点偏移；`enter_frame` 后调用者帧挂起，`last_off` 停在 CALL 站点不变。unwind 到调用者读 `frame.last_off` = CALL 站点，查其 `try_records` 命中覆盖该 CALL 的 try。IMPORT 模块体调用同理。

**`unwind_()` 统一读 `frame.last_off`**（顶帧由本轮循环顶写、外层帧由其挂起前最后一轮循环顶写），不需参数、不区分顶帧/外帧：

```cpp
Opt<Error> unwind_() {
    while (!ctx.frames().empty()) {
        auto& f = ctx.frames().top();
        if (auto rec = f.unit->find_try_handler(f.last_off)) { /* 命中,在此 unwind */ return nullopt; }
        ctx.exit_frame();   // 弹当前帧,复位值栈到调用者
    }
    return std::move(pending_error_);   // 全未命中 -> 未捕获
}
```

`raise_and_unwind_(Error)` / `throw_and_unwind_(Value)` 只置 `pending_*` 寄存器后调 `unwind_()`，**不收 `ip_off`、不写 `last_off`**（已由循环顶写好）。比「只在 CALL + raise helper 写」更简单：后者要把 ip_off 一路传进 helper 并在 helper 内写顶帧 `last_off`，且须枚举所有 raise 站点都走 helper；前者把写收口到唯一一处（循环顶），自然覆盖所有指令、所有 raise 站点。

---

## 坑 #3：CallFrame 不能有默认成员初始化（NSDMI）-- 破坏 trivially-copyable

**现象**：给 `CallFrame` 加 `u32 last_off = 0;`（默认成员初始化）后，`FrameStack<CallFrame>` 的 `static_assert(is_trivial_v<CallFrame>)` 失败。

**根因**：`FrameStack` 要求 T trivial + trivially-copyable + trivially-destructible（`static_assert` 三连）。NSDMI 会使类的默认构造函数变为非 trivial（即便初始化值是常量），导致 `is_trivial_v` 为 false。`CallFrame` 是 trivially-copyable 聚合，禁用 NSDMI。

**对策**：`CallFrame` 新字段**不写默认值**，初始化收敛进 `init_frame_`（Movement.cpp 已有此模式：`f.slots = ...; f.function = ...;` 逐字段赋值）。新增 `f.last_off = 0;` 即可。保持 `CallFrame` 为纯聚合（无 NSDMI、无用户构造函数）。

```cpp
struct CallFrame {
    ObjFunction* function;
    CodeUnit*    unit;
    ObjModule*   module;
    u8*          ip;
    Value*       slots;
    u32          last_off;   // 无 = 0！init_frame_ 里置
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
  const u16 catch_slot = declare_local_or_fail(*node->catch_param, node->loc());
  cur_fn_ctx()->mark_initialized(catch_slot);
  emit_stmt(node->catch_body.get());
  end_scope(line);
  patch_jump_or_fail(jskip, node->loc());
  // 回填占位项(编译末尾不再 sort):
  cur_cu()->try_records[rec_idx].end         = end;
  cur_cu()->try_records[rec_idx].handle      = handle;
  cur_cu()->try_records[rec_idx].stack_depth = stack_depth;  // try 入口快照
```

嵌套时内层 `visitTryStmtNode` 先于外层回填执行（内层整段在外层体内编译完），其预插占位 `begin_inner` > 外层占位 `begin_outer`，落在外层项之后 -> **整体按 begin 升序**，二分直接可用。`stack_depth` 在入口快照（try 体 `begin_scope` 前 `cur_fn_ctx()->locals_.size()`），与占位预插同处入口取值。

`find_try_handler` 的二分逻辑须返回**最内层**（最大 `begin` 且 `ip` 落 `[begin, end)` 的区间）——升序下即最后一个 `begin <= ip` 且 `ip < end` 的匹配项。

**核对（已读 `CodeUnit.cpp:141` 现有实现）**：当前二分**已是最内层语义**——先二分求 `low = 首个 begin > ip`，再从 `low-1`（begin 最大的 begin<=ip 项）向前找第一个 `end > ip`，即 begin 最大的覆盖区间，等价于「最内层」。**二分逻辑无需改动**。唯一改动：返回类型 `Opt<u32>`（仅 handle）→ `Opt<const TryRecord*>`（暴露 `stack_depth` 给 unwind，B1 已列）。前提「记录按 begin 升序」当前仅是注释假设（CodeGen 尚未发任何 try 记录），须由 B5 预插方案落实并实测嵌套 try 升序。

---

## 坑 #5：frame_depth 是运行时量，不能存进 TryRecord

**现象**：设计文档早期列出 `TryRecord.frame_depth`（try 所在的帧深度），但 CodeGen 编译期无法知道运行时调用栈深度（try 可能被任意深度的调用链触达）。

**根因**：`frame_depth` 是运行时量（unwind 时刻的调用栈深度），编译期不可定。

**对策**：**不存 `frame_depth`**。unwind 时从最内帧向外**遍历帧链**，每帧在其 `CodeUnit::find_try_handler(frame.last_off)` 查表，首命中即在该帧 unwind。帧深度由遍历隐式决定。`TryRecord` 只存编译期可知的 `stack_depth`（try 入作用域时已声明的局部数，相对 `frame.slots`）；catch 参数槽无需单独存（== `stack_depth`，见坑 #10）。

---

## 坑 #6：stack_depth 相对 frame.slots，不是栈基址

**现象**：unwind 截断值栈时用错基准 -> 截断到错误位置，破坏调用者帧的局部区。

**根因**：`stack_depth` 是「try 入作用域时**本帧**已声明的局部数」，相对 `frame.slots`（本帧槽 0），不是相对 `stack_base`（全局值栈基址）。跨帧 unwind 到 handler 帧时，要截断到 `handler_frame.slots + stack_depth`，丢弃 try 体内的临时值与本帧残留，但保留 handler 帧的 callee/参数/已声明局部。

**对策**：

```cpp
// unwind 命中 handler 帧 rec:
ctx.frames().truncate(handler_frame_index);                 // 弹掉内层帧
const auto new_top = handler_frame.slots + rec->stack_depth; // 相对 frame.slots
ctx.truncate_stack(static_cast<usize>(new_top - ctx.stack_base()));
handler_frame.ip = handler_frame.unit->code.data() + rec->handle;
ctx.push(thrown_value);   // 落在 slot stack_depth = catch 参数槽(值填槽,无需 STORE_LOCAL,见坑 #10)
```

`Movement::truncate_stack(usize new_size)` 断言 `new_size <= stack_size()`，置 `top_ = base + new_size`。

---

## 坑 #7：单寄存器模型 -- pending_error_ : Opt<Value> + ObjException 包运行时错误

**动机**：抛出的实体有两个侧面——catch 侧面要绑 aria 值保留类型（`throw 42` → e=Int 42）；run() 侧面未捕获要回结构化 `Error`（保 `ErrorCode` + 位置串，供 `test_ariavm` 9 处 `error().code()` 断言）。单一 `Opt<Error>` 会丢类型（throw 时刻值就压成串）；单一 `Opt<Value>` 会丢码（未捕获恒 `UncaughtException`）。解法：单寄存器 `Opt<Value>`，**运行时错误包成 `ObjException`（携码）**、**用户 throw 存原值（携类型）**，两种载荷同住一个 Value 寄存器。

**ObjException**（新 Object 子类型，`ObjType::EXCEPTION` 枚举早已预留）：
```cpp
class ObjException final : Object {
    ErrorCode  code_;     // 错误码
    ObjString* message_;   // 完整烘焙消息(含 "path:line:col: Category: Name detail",与 Error::message() 同形)
public:
    ErrorCode  code()    const noexcept { return code_; }
    ObjString* message() const noexcept { return message_; }   // catch 的 print(e)/str(e) 渲染此串
    void trace(GC& g) const noexcept override { mark_obj(g, message_); }   // 标 message_ GC 串
    String to_string() const override { return message_->view(); }        // print(e) -> 消息
};
```
工厂 `new_exception(gc, code, ObjString* message)`。`message_` 存**完整烘焙消息**（含位置前缀）——与双寄存器 catch 绑的懒合成串一致，位置不丢、catch UX 不退化。

**单寄存器 `pending_error_ : Opt<Value>`**（替代 `Opt<Error> + Opt<Value>` 双寄存器）：

| 路径 | 写入 pending_error_ | catch 绑定 | 未捕获 run() 回传 |
|---|---|---|---|
| 用户 `throw V` | `V`（原值，不包） | `V`（保类型：`throw 42`→e=Int 42） | `Error{UncaughtException, format_value(V)}` |
| 运行时错误 / native `fail` | `new_exception(gc, err.code(), new_string(gc, err.message()))`（包成 ObjException） | ObjException（`print(e)` 渲染消息；re-throw 保码） | `Error::with_message(ex.code(), ex.message()->view())`（原码+原消息含位置） |

**Error 需新增 `with_message(code, baked_string)` 工厂**：现有两个构造都经私有 `make_message` 重新烘焙（`Error(code, detail)` 会产出 `"Category: Name " + detail`，对已烘焙消息双重前缀）。`with_message` 跳过 `make_message`、直接装已烘焙串——唯一目的是从 ObjException 反提 `Error` 回传 run()。最小、收口于 Error.hpp。

**两硬约束都满足**：
- 类型保留：用户 throw 存原值，catch 绑原值 ✅。
- 码保留：运行时错误包成 ObjException（携 `code_`），未捕获经 `with_message` 回原码 ✅（`TypeMismatch`/`DivisionByZero`/`WrongArity`/`StackOverflow`/`UndefinedVariable`/`ModuleNotFound` 等 9 处断言过）。

**比双寄存器多的语义收益：re-throw 保码**。`try {1/0} catch(e) { throw e }` 未捕获：
- 双寄存器：catch 绑懒合成消息串 → `throw e` 存串 → 未捕获回 `UncaughtException`（**丢 DivisionByZero**）。
- 单寄存器：catch 绑 ObjException → `throw e` 存同一 ObjException → 未捕获 `with_message` 回 `DivisionByZero` ✅。

**catch 绑 ObjException 的 M3 可用性**：`print(e)`/`str(e)` 渲染消息 ✅；字符串拼接需 `"x" + str(e)`（`e` 非字符串，`e + "x"` 类型错）；`e.message()`/`e.code()` 留待 M5 方法/字段落地。M3 catch-of-runtime-error 的字符串操作多一个 `str()` 调用，可接受。

**站点改动**：`ctx_fail`/`call_native`/`AriaVM::raise(Error)` 现在置 `pending_error_ = Error` → 改为置 `pending_error_ = new_exception(gc, err)`（包一层）。`vm.fail(code, ...)` 助手内部包，原生函数调用点不变。`throw_and_unwind_(V)` 存原值不包。`unwind_` 命中 handler `push(*pending_error_)`（无需懒合成分支）。

---

## 坑 #8：pending_error_ 的 GC 安全 -- 不在值栈内，须由 tracer 显式标根

**现象**：`pending_error_ : Opt<Value>` 持 aria `Value`，运行时错误时装箱 `ObjException*`、用户 throw 时可能装箱任意 `Obj*`（如 `throw "msg"` 的 ObjString）。它存在 `Movement` 成员里，**不在值栈 `[base, top)` 区间内**，VM 根 tracer 默认只标值栈 + 帧的 function/module + modules_ + builtins_。raise 到 unwind 之间若触发 GC（如 `new_exception` 顶 `maybe_collect`、handler 处 `truncate_stack` + `push` 顶栈增长 `maybe_collect`），`pending_error_` 内的 Obj 无根可达 -> 被回收 -> catch 绑定到悬垂对象。

**对策**：`pending_error_` 必须纳入 VM 根 tracer。在 `AriaVM` 的 vm_roots tracer 里加：

```cpp
// 标挂起错误/抛出值(若持有 Obj，unwind 前可能跨 GC)
if (main_ctx_.has_error()) {
    mark_value(gc, *main_ctx_.pending_error());   // Value 可能是 ObjException* 或原 Obj*
}
```

`mark_value` 对 ObjException 会进一步 `ObjException::trace` 标其 `message_` ObjString。**不可遗漏**，否则 stress GC 下 try/catch 路径 use-after-free。

**构造期守卫**：`raise(Error)` 内 `new_exception(gc, err.code(), new_string(gc, err.message()))` 期间，`new_string`/`new_exception` 顶 `maybe_collect`，此时新对象尚未入 `pending_error_`，须用 `make_guard` 守护在建的 message/exception（沿用 `register_builtins` 的双守卫模式）；存入 `pending_error_` 后即经 tracer 根化，守卫释放。

**测试**：try/catch 路径必须开 stress GC（`gc.set_stress(true)`）验证根接线，复刻 `run_source` 已有的 stress 模式。

---

## 坑 #9：emit_stmt 对 UPtr<BlockNode> 须 .get()

**现象**：`visitTryStmtNode` 里 `emit_stmt(node->body)` 编译失败 -- `body` 是 `UPtr<BlockNode>`，`emit_stmt` 接 `StmtNode*`。

**对策**：`emit_stmt(node->body.get())`。`catch_body` 同理。低级但易忘。

---

## 坑 #10：catch 参数绑定走「值填槽」-- 无 catch_slot 字段、无 STORE_LOCAL 前导

**现象（化简前旧设计）**：旧 lowering 在 `TryRecord` 存 `catch_slot` 字段、handler 首条发 `STORE_LOCAL catch_slot`。但推一遍栈位发现两者皆冗余。

**根因**：项目的「值填槽」不变式（compile.md：`下个局部 slot = 当前栈高`，初始化值 push 后恰好落在该槽）在异常路径上同样成立：
- `stack_depth` = try 入口（try 体 `begin_scope` **之前**）局部数 = `cur_fn_ctx()->locals_.size()` 快照。try 体局部在 try scope 内声明，unwind 时丢弃（截到 `frame.slots + stack_depth`）。
- catch 参数在 try 体 `end_scope` **之后**声明 —— try 体局部已弹，下一个可用槽即 `stack_depth`，故 `catch_slot == stack_depth`，恒等。存进 `TryRecord` 是冗余字段。
- unwind 截到 `frame.slots + stack_depth`（top = `slots + stack_depth`）后 `push(thrown_value)` —— 值落在 `slots + stack_depth` = **slot `stack_depth` = catch 参数槽**。`e` 已被绑定。
- handler 首条若再 `STORE_LOCAL catch_slot`：peek 栈顶（`slots + stack_depth`）写回 slot `stack_depth`（`slots + stack_depth`）—— **自赋值 no-op**。该指令冗余。

**对策**：
1. `TryRecord` 只存 4 字段 `{begin, end, handle, stack_depth}`，去掉 `catch_slot`。
2. unwind 在 `truncate_stack` 后 `push(thrown_value)`，异常值自然落在 `e` 的槽（见坑 #6 示例）——handler 不发 `STORE_LOCAL` 前导，直接进 catch 体。
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
const u16 catch_slot = declare_local_or_fail(*node->catch_param, node->loc());
cur_fn_ctx()->mark_initialized(catch_slot);  // catch 体可读 e(== stack_depth);无 STORE_LOCAL
emit_stmt(node->catch_body.get());
end_scope(line);                     // 弹 catch 体局部 + e -> 栈高 stack_depth
patch_jump_or_fail(jskip, node->loc());      // 正常路径 -> L_end(此处栈高 stack_depth)
cur_cu()->try_records[rec_idx].end         = end;         // 回填占位项
cur_cu()->try_records[rec_idx].handle      = handle;
cur_cu()->try_records[rec_idx].stack_depth = stack_depth;
```

**栈平衡校验**：正常路径 try 体 `end_scope` 弹至 `stack_depth`；异常路径 catch 子句 `end_scope` 弹至 `stack_depth`（`e` + catch 体局部）；两路径在 `L_end` 均栈高 `stack_depth`，齐平。

**注意**：`catch_slot` 在 lowering 里仍由 `declare_local_or_fail` 拿到（catch 体经 `LOAD_LOCAL catch_slot` 读 `e`），但它 == `stack_depth`，**只编译期用、不入 `TryRecord`**。`mark_initialized` 在 `declare_local` 后立即调（`e` 由 unwind 的 push 在运行期填，编译期标已初始化以放行 catch 体的读检查）。

**核对（已读 `FunctionCtx.cpp` 现有实现）**：`end_scope_pop_count` 经 `pop_locals_deeper_than` **真正 `locals_.pop_back()` 移除**原 scope 局部（非只减 `scope_depth_`），故 `locals_.size()` 精确反映当前活局部、**无陈旧项堆积**。`stack_depth = cur_fn_ctx()->locals_.size()`（try 体 `begin_scope` 前快照）= try 入口活局部数 = 下一可用 slot = catch 参数槽。try 体 `end_scope` 弹回 `stack_depth`，catch 参数 `add_local` 后 `locals_.size() == stack_depth + 1`、catch 参数 slot == `stack_depth`。定稿成立，无问题。

---

## 坑 #11：run_() 错误传播重构 -- 命中 handler 后必须 continue 重取 frame 引用

**现象**：把 `return runtime_err(...)` 改为 raise+unwind 后，命中 handler 的路径若不 `continue` 而是继续 `break`/落到 switch 尾部，会触用已被 `frames_.truncate` 弹掉的旧帧引用 -> use-after-free。

**根因**：`run_()` 主循环 `switch (CallFrame& frame = frames.top(); ...)`。`unwind_()` 命中 handler 时 `frames_.truncate` 可能弹掉多帧，`frame` 引用悬垂。必须 `continue` 回循环顶重新取 `frames.top()`。

**对策**：统一模式：
```cpp
if (auto u = raise_and_unwind_(Error{...})) {
    return runtime_err(std::move(*u));   // 未捕获 -> 终止 run_
}
continue;                                // 命中 handler -> frame 已废，循环顶重取
```

约 25 处 `return runtime_err(...)` 站点（算术/比较/NEGATE/LOAD|STORE_GLOBAL/CALL/IMPORT/THROW）改为此模式。**每处都不能漏 `continue`**。

`call_value` 失败站点：
```cpp
if (!call_value(ctx, callee, argc)) {
    if (auto u = unwind_()) return runtime_err(std::move(*u));  // call_native/ctx_fail 已置 pending_error_
    continue;
}
```

---

## 坑 #12：THROW 指令语义 -- 弹值后 throw_and_unwind_

```cpp
case OpCode::THROW: {
    Value v = ctx.pop();
    if (auto u = throw_and_unwind_(std::move(v))) {
        return runtime_err(std::move(*u));
    }
    continue;
}
```

`throw_and_unwind_(Value v)`：**直接存原值** `pending_error_ = v`（不包 ObjException——用户 throw 携类型，catch 绑原值）+ 调 `unwind_()`。返回 `Opt<Error>`（nullopt = 已 dispatch 到 handler，some = 未捕获回传）。未捕获时 run() 见 pending_error_ 是原值（非 ObjException）→ 回 `Error{UncaughtException, format_value(v)}`。

---

## 坑 #13：unwind_() 帧遍历 -- 无 handler 的帧用 exit_frame 逐帧弹

unwind 从最内帧向外遍历：
1. 当前帧：用 `frame.last_off`（由本轮循环顶写入，见坑 #2）查 `frame.unit->find_try_handler`。
2. 命中 -> 在该帧 unwind（truncate 帧栈到该帧 + truncate_stack 到 `slots + stack_depth` + `push(*pending_error_)`（Value 原样:ObjException 或用户 throw 原值，落在 catch 参数槽 stack_depth，见坑 #10）+ `ip = handle` + 清 `pending_error_`）-> 返 nullopt。
3. 未命中 -> `ctx.exit_frame()`（弹该帧 + 复位值栈到该帧 slots，丢弃 callee+args，恢复调用者栈态）-> 继续遍历调用者帧，用 `frame.last_off`（CALL 站点，见坑 #2）查表。
4. 所有帧都无 handler -> 从 `pending_error_` 反提 `Error`（ObjException 经 `Error::with_message(code, message)`、原值经 `Error{UncaughtException, format_value(v)}`），清 `pending_error_`，返回该 Error。

**注意**：遍历中 `exit_frame` 修改帧栈，循环索引/引用要小心。建议用 `while (!frames.empty())` + 每轮取 `frames.top()`，命中即停，未命中 `exit_frame` 后下一轮。

**核对（已读 `Movement.hpp:141` 现有实现）**：`exit_frame` 先取被弹帧 `slots`（= callee 位置 = 调用者在 CALL 前的栈顶 = 调用者局部区末尾），`frames_.pop()` 后 `set_stack_top_(slots)` 把值栈 top 复位到该处——即**调用者栈态**（调用者局部区 `[调用者.slots .. slots)` 完好保留，被调用者的 callee/args/局部全丢弃）。逐帧 `exit_frame` 逐步回退，每步调用者局部区都在新 `top` 之下完好。`FrameStack::truncate(idx)` 只改帧栈 size、不动值栈（值栈是 `Movement::buf_`，帧栈是独立 `frames_`），故命中分支须手动 `truncate_stack`（坑 #6 已写明）。定稿成立，无问题。

---

## 坑 #14：Movement 改单寄存器 pending_error_ : Opt<Value> + truncate_stack

`Movement`（`runtime/Movement.hpp`）改造：
- `pending_error_` 类型从 `Opt<Error>` **改为 `Opt<Value>`**（单一挂起寄存器，装 ObjException 或原值，见坑 #7）。配套访问器 `raise(Value)`/`has_error()`/`take_error()`（返 `Opt<Value>`）/`clear_error()` 四件套语义不变，仅载荷类型变。
- `raise(Error)` 的语义改为「包成 ObjException 再存」：`pending_error_ = Value::from_obj(new_exception(gc, err.code(), new_string(gc, err.message())))`——但 `Movement` 不持 `GC&`，故**包的过程上移到 `AriaVM`**：`AriaVM::raise(Error)`/`AriaVM::fail(...)` 负责构造 ObjException + `ctx.raise(Value)` 存入；`Movement` 只暴露 `raise(Value)`（存原值，不包）。`ctx_fail`/`call_native` 经 `vm.fail(...)` 间接包，调用点不变。
- 新增 `void truncate_stack(usize new_size) noexcept`（断言 `<= stack_size()`，置 `top_ = base + new_size`；复用私有 `set_stack_top_` 语义，对外收紧「unwind 截值栈」）。
- `reset()` 清 `pending_error_`（原 `Opt<Error>` 清空逻辑同名同义，类型改 `Opt<Value>`）。
- 删除原 `pending_throw_` 及其四件套（不再需要——单寄存器）。

`pending_error_` 的 GC 标根见坑 #8。

---

## 实现顺序建议（下次重启时）

1. **B0** 新增 `ObjException : Object{ErrorCode code_, ObjString* message_}`（`object/ObjException.hpp/.cpp`）+ 工厂 `new_exception(gc, code, ObjString* message)` + `trace`（标 message_）+ `to_string`（返 message_->view()）；`ObjType::EXCEPTION` 枚举与 `to_string(ObjType)` 早已预留，无需改枚举。`Error` 新增 `with_message(code, baked_string)` 工厂（跳过 `make_message`，供 run() 反提，坑 #7）。`format_value`/`format_value_debug`/`type_name` 加 EXCEPTION 分支。
2. **B1** `TryRecord` 扩字段 `{begin, end, handle, stack_depth}`（4 字段,无 `catch_slot`，见坑 #10） + `find_try_handler` 返 `Opt<const TryRecord*>`（`CodeUnit.hpp/.cpp`；二分已是最内层语义，仅改返回型，坑 #4 核对）。
3. **B3** `Movement` 改 `pending_error_` 为 `Opt<Value>` + 加 `truncate_stack` + `raise(Value)`（存原值不包）+ `reset` 清；删除 `pending_throw_` 计划（`Movement.hpp/.cpp`，坑 #14）。
4. **B2** `CallFrame` 加 `u32 last_off`（**无 NSDMI**，`init_frame_` 置 0，坑 #3）。
5. **B4** `AriaVM` 加 `unwind_`/`raise_and_unwind_(Error)`（内部 `new_exception` 包成 Value 存入）/`throw_and_unwind_(Value)`（存原值不包）；`raise(Error)`/`fail(...)` 改为包 ObjException；vm_roots tracer 标 `pending_error_`（坑 #8）；`run_()` 循环顶每轮取指前写 `frame.last_off = offset(frame.ip)`（坑 #1/#2）；未捕获从 `pending_error_` 反提 `Error`（ObjException 经 `with_message`、原值经 `Error{UncaughtException, format_value}`）。
6. **B5** CodeGen `visitTryStmtNode`/`visitThrowStmtNode` 发射（坑 #9/#10）；`visitTryStmtNode` **入口预插占位 + 结尾回填** `try_records`（构造即升序，**不**编译末尾排序，坑 #4）。
7. **B6** Disassembler `try_records:` 小节。
8. **B7** 测试（含 stress GC，坑 #8；含 re-throw 保码用例）。

每步可 `clang++ -std=c++23 -I src -fsyntax-only` 单独验证，B4/B5 后跑端到端。

---

## 测试矩阵（B7）

- 正向：throw 被 catch 捕获；catch 绑定值保类型（`throw 42` -> e==42 Int）；未捕获 throw -> `UncaughtException`；嵌套 try（内层捕获 / 外层捕获内层 rethrow）；catch 内再 throw；跨帧捕获（被调函数 throw、调用者 try 捕获）。
- 运行时错误可捕获：`try { 1/0 } catch (e) {}`（e 绑定 `ObjException`，`print(e)` 渲染消息）；`try { len(nil) } catch (e) {}` 捕获 native fn 错误（e 是 ObjException，`e.code()` 待 M5）。
- **re-throw 保码（单寄存器语义收益）**：`try { 1/0 } catch (e) { throw e }` 未捕获 -> `run()` 回 `Error{DivisionByZero,...}`（非 `UncaughtException`）；对比用户 `throw 42` 未捕获 -> `UncaughtException`。
- 反向：`ErrTryWithoutHandler` 既有保留；含 finally 的 try 报 `NotImplemented`（M3b 占位）。
- stress GC：try/catch 路径 `gc.set_stress(true)` 验根接线（坑 #8，标 `pending_error_`）。
- 既有 `NativeFnSideChannelError` 仍期望 `TypeMismatch`（单寄存器 + ObjException 保码：未加 try 时运行时错误包成 ObjException 存入 `pending_error_`，未捕获经 `with_message` 回原码，坑 #7）。

---

## 附：本次回退范围

M3 实现已整体回退至 HEAD，仅保留 M2（内置函数注册，`src/runtime/Builtins.{hpp,cpp}` + AriaVM 构造期一次性注册进 VM 级 `builtins_` 表 + `LOAD_GLOBAL` 模块 globals 未命中回退查之 + `CMakeLists.txt` + 12 个 builtin 测试）。M3 相关改动（`CodeUnit`/`Movement`/`AriaVM`/`CodeGen`/`Disassembler`/测试）全部回到 HEAD，等重启时按上述顺序重做。完整 M3 设计上下文见归档计划 `/Users/icelake/.claude/plans/quizzical-sniffing-cascade.md`（决策 D1-D8）。