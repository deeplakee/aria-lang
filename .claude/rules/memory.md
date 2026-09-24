---
name: aria-memory
description: aria 解释器 memory 层模块参考：Buffer/Array（trivial 可增长容器）、Allocator 约束、HashTable（Swiss Table）、InternPool 驻留池、GC（mark-sweep/临时根/VM 根 tracer/核心不变式）。读写 src/memory/** 或涉及分配器、GC 触发点与守卫纪律时使用。
paths:
  - "src/memory/**"
---

# memory 层模块参考

## `memory/Buffer.hpp`

- `Buffer<T, Alloc = GC>`（`TriviallyCopyable`/`TrivialAllocator` 约束）：只持 `{alloc, data, cap}` 的最小可增长缓冲底座，集中 `allocate`/`reallocate`/`deallocate`，**无逻辑长度**。
- `reserve(new_cap)` 仅在 `new_cap > cap_` 时经 `alloc->reallocate<T>` 搬迁，**不返回基址差**（算 delta 是 UB）--有派生裸指针的调用方（Movement 值栈）须在前后各取一次 `data()`、以整数偏移重建（详见 `Movement::grow_stack_`）。
- 非拷贝/非移动。`Array` 与 `Movement` 值栈均建于其上；不适用于内容需重定位的容器（HashTable rehash / InternPool rehash 直接用分配器）。

## `memory/Array.hpp`

- `Array<T, Alloc = GC>`：可扩容 trivial 数组（Buffer 底座 + 逻辑长度 `len_`）；扩容策略固定内联--初始 `kInitialCapacity = 8`、2x 几何增长，无 GrowPolicy 模板参数。
- **迭代器**为裸指针（存储连续，兼容 range-for 与 `<algorithm>`），push/resize/reserve 扩容搬迁即失效（std::vector 同语义）；mark-sweep 不搬块且 allocate/reallocate 永不触发 GC，迭代期间发生对象分配/GC 不影响缓冲。
- 用途：顺序增长用（ObjList 元素 / CodeUnit 字节码）；HashTable 不走它（rehash 不能 memcpy）。非拷贝/非移动。

## `memory/Allocator.hpp`

- `TriviallyCopyable` / `TrivialAllocator` concept（后者以 `u8` 为代表类型校验 `allocate<T>`/`reallocate<T>`/`deallocate<T>`）+ `class GC;` 前向声明。
- `Buffer`/`Array`/`HashTable` 经此与具体分配器**解耦**：容器头不 include `GC.hpp`，使用 GC 作分配器的具体类（`AriaArray`/`AriaHashTable`/`CodeUnit`/`Movement`/`InternPool`）自行 include（`InternPool` 经 ctor 函数体内 `static_assert(TrivialAllocator<Alloc>)` 约束--GC 为其值成员拥有者，类体内处不完整类型，约束延后到实例化点）。

## `memory/HashTable.hpp`

通用 Swiss Table 模板 `HashTable<K,V,Hash,Eq,Alloc = GC>`（值无关，不依赖 Value；`HashFunctor`/`EqFunctor` concept 约束 Hash/Eq 为可默认构造的 noexcept 函数对象）。

- ctrl 字节编码（0xFF 空 / 0xFE 墓碑 / 0x00..0x7F 占用+h2）+ h1/h2 拆分 + 2 的幂 cap（`kInitialCap = 8`）+ 三角探测 + 7/8 负载因子。
- `set`（命中覆写/未命中插入）/`find`（返 `Entry*`）/`erase`/`clear` + 只读 `const_iterator`（跳空槽/墓碑，AriaHashTable trace 与 ObjMap 迭代走此通道）。
- 两块独立分配（`ctrl_` + `entries_`），rehash 一起重分配。不可拷贝/不可移动，持 `Alloc*`。

## `memory/InternPool.hpp`

header-only 模板 `InternPool<Alloc = GC>`，无 .cpp：字符串驻留池（**weak root**，字符串专用 set--不进 `mark_roots_`，`collect` 在 sweep 前调 `remove_white()` 摘除指向白色串的表项防 sweep 后悬垂）。

- 裸 `ObjString** slots_` + 低位标签（nullptr 空 / `0x1` 墓碑 / 真指针占用），8B/槽（`kInitialCap = 8`），无 ctrl/h2（靠内容比较）；`find`/`insert`/`remove_white`。
- 头循环（GC 持值成员 <-> InternPool 用 `GC*`）经模板延后具现化 + ctor 函数体内 `static_assert` 打破（同 `Object.hpp` 对 GC 的处理）。

## `memory/GC.hpp` / `.cpp`

GC 分配器（`allocate<T>`/`deallocate<T>`/`reallocate<T>` 模板，按 T 计数）+ 对象链表 + `new_object<T>`/`delete_object`（私有，`new_object` 的逆，供 `sweep_`/`free_all_` 调）。

**公共入口与设施**

- mark-sweep 入口 `mark_value`/`mark_object`/`maybe_collect`/`collect`；临时根 `push_temp_root` + `Guard` RAII / `make_guard`（`Guard::push` 增量压入）；禁用锁 `disable_gc`/`enable_gc`（`u32 lock_count_` 计数，支持嵌套）+ `LockGuard`/`make_lock`/`is_gc_disabled()`。
- stress 开关 `set_stress`（测试用，开启后每次 `new_object` 强制 collect）；`bytes_allocated()`（**存活**字节、随回收回落）与 `allocation_count()`（累计 `new_object` 次数、单调不减--给基准确定性分配读数，见 `bench/vm_bench.cpp`）。
- 常量 `kInitialGcThreshold = 1024*4`、`kGcGrowFactor = 2`；`gray_stack_`/`temp_roots_` 用 `List`（scratch，不计 `bytes_allocated_`）；`intern_.slots_` 计入 `bytes_allocated_`。
- `InternPool intern_` 成员（`intern_find`/`intern_insert` 委托；`collect` 在 sweep 前调 `intern_.remove_white()`）。

**VM 根 tracer**

`set_vm_roots(std::function<void(GC&)>)`，AriaVM 注册，collect 时调 lambda 标根：

- `modules_`（解释器级共享模块表）+ `builtins_`。
- `registers_`（值寄存器组 = `List<Object*>`，VM 单例对象的统一存放表，Object 根类在其中；`hook_vm_roots` 一趟循环逐格 mark_object）。
- `string_constants_`（常量串表，一趟循环 mark_object--驻留池是 weak root，不标根下轮 collect 即摘除）。
- `current_` 一点：执行上下文（`Movement : Object`）已入对象链表，其 `trace` 自标值栈 `[base, top)`（run() 期局部/实参/临时值，最关键的根；run() 外为空态）、各活动帧 `closure`（级联标 function/upvalues）与 `module`、挂起错误寄存器、open upvalue 开链节点（「闭包已死而 upvalue 仍在链」的悬垂防线），并经 `mark_object(previous_)` 沿 resume 链级联。

组合而非继承，GC 不识 VM 类型（经 `set_vm_roots` 回调接入）。

**核心不变式**

`allocate<T>`/`reallocate<T>` **永不触发 GC**；GC 仅在 `new_object` 顶部 `maybe_collect` 与 VM safe point 触发。

这是与「link-on-alloc + publish-after」对象模型绑定的定义性约束，非性能取舍--`new_object` 返回的对象此刻白色无根，需发布进根才安全，而发布动作本身是 trivial 分配；若该分配触发 GC 会扫掉白色无根对象致悬垂。故 `add_constant`/`intern_insert`/`globals().set` 等「fresh 对象跨 trivial 分配再发布」写法免守卫全靠此。`allocate`/`reallocate` 是叶函数，靠契约注释 + review 守；另一方向（裸持白色对象跨真 GC 点漏 `make_guard`）靠显式守卫 + stress GC 测试守。
