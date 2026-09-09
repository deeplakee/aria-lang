# M4 闭包实施计划

> 状态：**已定稿、未开始实施**（2026-09-07）。分四阶段推进，每阶段结束可独立绿提交（build + ctest 全绿）；阶段划分即推进节奏，可穿插其他基建工作分步执行。M4 开工前重读 `exception-implementation-pitfalls.md`（CLAUDE.md 纪律：异常相关特性重启前重读）。
> 行号锚点基于定稿时 HEAD（commit `0c17468`），后续基建改动会使行号漂移，定位以符号/描述为准。
>
> **决策记录（2026-09-07）**：defer 善后机制**移出 M4**，降级为「其他功能完成后的可选项」（优先级最低），不再绑定任何里程碑。M4 只做闭包本体。全部「defer 随 M4」表述已随本计划定稿同步（grammar.txt 裁撤记录段 / vm-design §6 / bytecode-instruction-set.md / exception-implementation-pitfalls.md / rules·compile+runtime / CLAUDE.md·README / 源码与测试注释）。

## 1. 语义模型（定调）

捕获即引用（Lua/clox 语义）：内层函数引用外层局部，持一个指向该栈槽的 upvalue，外层后续修改对内层可见；局部所在作用域/帧退出时才「关闭」（把值迁进 `ObjUpvalue.closed`）。与 grammar.txt:193 既定解析顺序「局部 -> upvalue -> 模块全局」吻合。

## 2. 三个关键设计决策

1. **Upvalue 用指针式表示 + 按槽址降序的开链**：open 态持 `Value* location`（指入值栈），closed 态持值。`grow_stack_` 增第三类重绑（搬运前走链记偏移、搬运后重算，与现有 top_/slots 同法，约 8 行）。理由：指令集 §4.4「开指栈槽」、gc-implementation-plan Phase 4、vm-design §4.1 全部按此模型预写，且 clox/Wren/Lua 皆指针式；热路径 LOAD/STORE_UPVALUE 零额外算术。索引式省重绑但每次访问多一次加法且偏离全部文档，不取。
2. **捕获描述表存 ObjFunction 元数据**（指令集 §4.13 已定稿，照办）：`ObjFunction` 持 `Array<UpvalueDesc>`，每条 `{is_local: bool, index: u16}`；`is_local=true` 捕外层帧槽 index，`false` 穿透复用外层闭包的 upvalue index。不进字节码流，`CLOSURE` 保持定长 3B（ConstU16），Disassembler 零改动。
3. **callable 收敛为闭包**：`CallFrame.function: ObjFunction*` -> `ObjClosure* closure`（顶层入口也闭包，`run_function` 内包一个空 upvalue 的闭包进帧）；翻转后 `call_value` 删 FUNCTION 直调分支（`ObjFunction` 退为常量池内部物，不再以可调用值上栈）。迁移期临时保留一个「FUNCTION 现场包闭包」分支，使旧 lowering 与既有测试在阶段 2 后仍然全绿，阶段 3 随编译翻转删除。

## 3. 实施阶段

### 阶段 1：object 层

- **新增 `src/object/ObjUpvalue.{hpp,cpp}`**（`final : Object`，`ObjType::UPVALUE` 枚举已有）：字段 `Value* location_`（open）/ `Value closed_`（ctor 播 `nil_val`）/ `ObjUpvalue* next_`（链）；`is_open()`、`value_slot()`（open -> `location_`，closed -> `&closed_`，LOAD/STORE 统一走它）、`close()`（`closed_ = *location_; location_ = &closed_`）、`set_location()`（重绑用）；`trace` 标 `*value_slot()`；工厂 `new_upvalue(GC&, Value* slot)`（入参是裸栈槽无对象可守，调用方链入后即经 VM 根 tracer 为根）。
- **新增 `src/object/ObjClosure.{hpp,cpp}`**（`final : Object`）：字段 `ObjFunction* function_` + `Array<ObjUpvalue*> upvalues_`（VM 逐个后填）；`trace` 标 function + 全部 upvalue；`to_string()` 委托 `function_->to_string()`（`<fn name>`，trace_execution 兼容）；equals 保持默认地址相等；工厂 `new_closure(GC&, ObjFunction*)` 不守卫入参（每方只守自己创建的）。
- **`Object.hpp`**：`ObjType` 追加 `CLOSURE`（尾部）+ `to_string(ObjType)` 补 case（default 是 `UNREACHABLE()`，漏加即死）；`Value.cpp` `format_value_debug` 补 CLOSURE/UPVALUE 渲染分支。
- **`ObjFunction`**：新增 `UpvalueDesc{bool is_local; u16 index}`（定义于 ObjFunction.hpp）+ `Array<UpvalueDesc> upvalue_descs_` + 读口/setter（编译期一次性 flush；~ObjFunction 级联释放）。
- **登记**：根 CMakeLists.txt aria_core object 组按字母序插两对文件；tests/CMakeLists.txt `# object` 组插两个测试文件。
- **测试**：`tests/object/test_objupvalue.cpp`（开/闭转换、value_slot、trace stress 存活）、`test_objclosure.cpp`（包 fn、upvalue 数组、trace stress、type_name）；`rules/object.md` 子类型清单同步。

### 阶段 2：VM 机制（runtime 全量，迁移分支保绿）

- **`Movement`**：
  - `CallFrame.function` 改名 `closure`（`ObjClosure*`；trivially-copyable 约束不破）；`enter_frame(ObjClosure*, argc)`，`init_frame_` 的 unit/module 从 `closure->function()` 取。
  - 开链：`ObjUpvalue* open_upvalues_` 链头 + `find_open_upvalue(Value* slot)`（降序链查等值复用）、`link_open_upvalue`（降序插入）、`close_upvalues(Value* from)`（闭所有 `location >= from`：迁值 + 摘链）、只读遍历口（tracer 用）。
  - `grow_stack_`（Movement.hpp:238-262）：第三类重绑——搬运前走链把各 `location_ - old_base` 记入临时 `List<usize>`（链序两趟间稳定），搬运后逐节点 `set_location(new_base + offset)`；同步删「M4 落地后须重绑」预告注释（Movement.hpp:36-38、236-237）。
  - `reset()`：先 `close_upvalues(buf_.data())` 再清场（HALT 不弹帧的收场安全网，防开指残留跨 run 复用栈区）。
- **`AriaVM`**：
  - `run_function`（AriaVM.cpp:442-456）：入口 fn 先包空闭包（`make_guard` 跨 `new_object`）再压栈进帧——顶层也闭包，`run(ObjFunction*)` 公开签名不动。
  - `call_value`（AriaVM.cpp:458-472）：+ `case CLOSURE -> call_closure`（arity/frames 检查照 call_function）；`case FUNCTION` 改临时 wrap（现场 `new_closure` + guard + 进帧），阶段 3 删。
  - 四 opcode 实装（替换 AriaVM.cpp:776-781、1066-1067 的 `not_implemented`）：
    - `CLOSURE`：u16 常量取 fn（`as<ObjFunction>`）-> `new_closure` 入 guard -> 遍历 `fn->upvalue_descs()`：`is_local` -> 槽址 = `frame.slots + index`，`find_open_upvalue` 复用否则 `new_upvalue` + `link`；否则复制 `frame.closure->upvalues()[index]`；逐个 push 进闭包数组（Array push 走 trivial 分配不触 GC，靠 GC 核心不变式免逐个守卫）-> 压闭包值。
    - `LOAD_UPVALUE idx`：`push(*closure->upvalues[idx]->value_slot())`。
    - `STORE_UPVALUE idx`：peek-store（写 `value_slot()` 留栈顶值，与 STORE_LOCAL 同形）。
    - `CLOSE_UPVALUE`：`close_upvalues(top_ - 1)` + `pop()`（指令集 §4.4 语义：关指顶槽的 upvalue 并弹顶）。
  - 三处关闭挂点：RETURN 在 `exit_frame`（AriaVM.cpp:1187）前 `close_upvalues(frame.slots)`；`unwind_` handler 命中在 `truncate_stack`（AriaVM.cpp:684）前 `close_upvalues(slots + stack_depth 的槽址)`；未命中在 `exit_frame`（AriaVM.cpp:691）前 `close_upvalues(frame.slots)`。
  - vm_roots tracer（AriaVM.cpp:280-298）：沿 `current_` 链逐 Movement 标开链各节点（`mark_object`）——防「闭包已死而 upvalue 仍在链」的悬垂（clox 已知坑；vm-design M6 trace 清单本就含「open upvalue 链」）；同步删 AriaVM.hpp:41「open upvalues 留待 M4」类预告注释。
  - 消费点改写：trace_execution（AriaVM.cpp:229）与 RETURN 模块帧判定（AriaVM.cpp:1186）改走 closure（`closure->function()->name()`）。
- **测试**（tests/runtime/test_ariavm.cpp，手写 emit）：捕获读/写、同槽捕获复用同一 `ObjUpvalue`、CLOSE 后读已迁值、open upvalue 下压 2048 值触发增长后仍读对（扩 `StackGrowsAndRebasesFrames` 模式）、unwind 跨帧关闭、AriaVMStress 下开链存活；`NotImplemented` 用例若占用了四 opcode 之一改用仍 fatal 的 M5 指令（MAKE_* 系）。
- `rules/runtime.md`、`rules/memory.md` 同步（开链/重绑/tracer 落地状态）。

### 阶段 3：compile 翻转

- **`FunctionCtx`**：`List<UpvalueDesc> upvalues_` + `add_upvalue(desc) -> Opt<u8>`（同 `(is_local,index)` 去重复用；>255 返空）。
- **`CodeGen`**：
  - `resolve_name_or_fail`（CodeGen.cpp:160-175）：Upvalue 占位分支接真递归 `resolve_upvalue`：enclosing 局部命中 -> 置该 ctx `Local.is_captured = true` + `{is_local=true, slot}`；否则递归 enclosing 的 upvalue -> `{is_local=false, idx}`；`add_upvalue` 满 -> `fail(TooManyUpvalues)`。
  - `emit_load_var`/`emit_store_var` Upvalue case（CodeGen.cpp:275-276 / 293-294）：`LOAD_UPVALUE`/`STORE_UPVALUE`（u8 索引；不做 init 检查——捕获时序语义同 Lua，与全局路径一致）。
  - `compile_function`：`LOAD_CONST fn_idx`（CodeGen.cpp:383-384）-> `CLOSURE fn_idx`；成功尾部把 `child->upvalues_` flush 进 `fn->upvalue_descs()`（发射先于子上下文创建不碍事——描述表在 ObjFunction 元数据，不在字节码流）。
  - 新助手统一弹区清理发射（实施终态定名 `emit_pop_locals_to(target, line)`，取代旧「仅计数发射」的 pop_locals_to 助手；退出作用域 = 先发射 + `FunctionCtx::end_scope()` 收尾）：弹区局部自栈顶（最内）向外遍历，整区一条 `POP_N`（被捕获局部一并计数），弹区含被捕获局部才追加一条 `CLOSE_UPVALUE`（实施中定夺改语义为 Lua `OP_CLOSE` 式批量关闭：关闭所有槽址 >= 新栈顶的开 upvalue、无弹栈，弹区槽已在新栈顶之上不 push 不覆写即安全）。落点：`visitBlockNode` end_scope（CodeGen.cpp:464）、for/for-in 作用域出口（554/605）与 **for-in per-iteration 出口（596，每轮新鲜绑定语义）**、break/continue（614/624）、try 两 end_scope（690/699）。
- **`ErrorCode.hpp`**：+ `TooManyUpvalues`（Semantic 类，照 TooManyParameters 模式：枚举 + to_string + category 映射）。
- **`call_value` 删临时 FUNCTION wrap 分支**，default 的 CallNonCallable 消息更新（"closures / native functions"）；tests/runtime/test_ariavm.cpp 手写站点 `LOAD_CONST fn + CALL` -> `CLOSURE fn_idx + CALL`。
- **测试**（tests/compile/test_codegen.cpp 端到端 + 反汇编文本）：计数器闭包（路线表验收样例：`fun make_counter() { var n = 0; return fun() { n = n + 1; return n; }; }`）、双闭包共享同一 upvalue、捕获后外层改值内层可见（引用语义）、块出作用域后闭包读已关值、嵌套具名 fun 递归自捕获（名字是外层局部经 upvalue 回递）、unwind 后幸存闭包读值；反汇编断言 CLOSURE 出现 / `LOAD_CONST fn` 消失 / per-iteration CLOSE_UPVALUE 位置；编译错 TooManyUpvalues（程序生成 256 个捕获的源码）。
- 指令集文档 §4.4/§4.13/§5.4 标落地，修 §5.4 示例（删 `LOAD_CONST fn_idx` 行——定夺为 CLOSURE 自取常量，LOAD_CONST 行是旧 lowering 残留）；`rules/compile.md`、`rules/bytecode.md` 同步（捕获解析、CLOSE_UPVALUE 发射点、CLOSURE lowering）。

### 阶段 4：文档收尾 + 全量验证

- vm-design §6 M4 行更新落地状态；§3 草图 CallFrame 注释、§4.1 重绑说明同步。
- gc-implementation-plan Phase 3 行记 ObjClosure/ObjUpvalue 落地。
- CLAUDE.md / README 进度行：M4 闭包已落地，待续 M5 类 / M6 协程（defer 为可选后续）。
- 坑点文档：实施中发现的 unwind-close 坑点补录（M4 的 RETURN/unwind 关闭挂点与 M3 的截栈/弹帧交互是坑点高发区）。
- 全量验证：默认 NaN-boxing 与 `build/tagvalue` 双配置 ctest 全绿；`--eval` 冒烟计数器/共享状态/块捕获/递归嵌套 fun/异常跨帧样例。

## 4. 验收

- 路线表 M4 标准：计数器闭包等经典样例正确。
- 双值表示配置 ctest 全绿（机制测试覆盖：捕获读/写/复用/关闭、unwind 关闭、栈增长重绑、stress GC 开链存活）。
- 无新增 opcode（四条早已预留，`kOpCodeCount=63` 不变），Disassembler 零改动。