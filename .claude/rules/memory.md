---
name: aria-memory
description: aria 解释器 memory 层模块参考：Buffer/Array（trivial 可增长容器）、Allocator 约束、HashTable（Swiss Table）、InternPool 驻留池、ShellPool 对象壳池、RawAlloc 后端原语、GC（mark-sweep/临时根/VM 根 tracer/核心不变式）。读写 src/memory/** 或涉及分配器、GC 触发点与守卫纪律时使用。
paths:
  - "src/memory/**"
---

# memory 层模块参考

## `memory/Buffer.hpp`

- `Buffer<T, Alloc = GC>`（`TriviallyCopyable`/`TrivialAllocator` 约束）：只持 `{alloc, data, cap}` 的最小可增长缓冲底座，集中 `allocate`/`reallocate`/`deallocate`，**无逻辑长度**。
- `reserve(new_cap)` 仅在 `new_cap > cap_` 时经 `alloc->reallocate<T>` 搬迁，**不返回基址差**（算 delta 是 UB）--有派生裸指针的调用方（ObjMovement 值栈）须在前后各取一次 `data()`、以整数偏移重建（详见 `ObjMovement::grow_stack_`）。
- 非拷贝/非移动。`Array` 与 `ObjMovement` 值栈均建于其上；不适用于内容需重定位的容器（HashTable rehash / InternPool rehash 直接用分配器）。

## `memory/Array.hpp`

- `Array<T, Alloc = GC>`：可扩容 trivial 数组（Buffer 底座 + 逻辑长度 `len_`）；扩容策略固定内联--初始 `kInitialCapacity = 8`、2x 几何增长。
- **迭代器**为裸指针（存储连续，兼容 range-for 与 `<algorithm>`），push/resize/reserve 扩容搬迁即失效（std::vector 同语义）；mark-sweep 不搬块且 allocate/reallocate 永不触发 GC，迭代期间发生对象分配/GC 不影响缓冲。
- 用途：顺序增长用（ObjList 元素 / CodeUnit 字节码）；HashTable 不走它（rehash 不能 memcpy）。非拷贝/非移动。

## `memory/Allocator.hpp`

- `TriviallyCopyable` / `TrivialAllocator` concept（后者以 `u8` 为代表类型校验 `allocate<T>`/`reallocate<T>`/`deallocate<T>`）+ `class GC;` 前向声明。
- `Buffer`/`Array`/`HashTable` 经此与具体分配器**解耦**：容器头不 include `GC.hpp`，使用 GC 作分配器的具体类（`AriaArray`/`AriaHashTable`/`CodeUnit`/`ObjMovement`/`InternPool`）自行 include（`InternPool` 经 ctor 函数体内 `static_assert(TrivialAllocator<Alloc>)` 约束--GC 为其值成员拥有者，类体内处不完整类型，约束延后到实例化点）。

## `memory/HashTable.hpp`

通用 Swiss Table 模板 `HashTable<K,V,Hash,Eq,Alloc = GC>`（值无关，不依赖 Value；`HashFunctor`/`EqFunctor` concept 约束 Hash/Eq 为可默认构造的 noexcept 函数对象）。

- ctrl 字节编码（0xFF 空 / 0xFE 墓碑 / 0x00..0x7F 占用+h2）+ h1/h2 拆分 + 2 的幂 cap（`kInitialCap = 8`）+ 三角探测 + 7/8 负载因子。
- `set`（命中覆写/未命中插入）/`find`（返 `Entry*`）/`erase`/`clear` + 只读 `const_iterator`（跳空槽/墓碑，AriaHashTable trace 与 ObjMap 迭代走此通道）。
- 两块独立分配（`ctrl_` + `entries_`），rehash 一起重分配。不可拷贝/不可移动，持 `Alloc*`。

## `memory/InternPool.hpp`

header-only 模板 `InternPool<Alloc = GC>`，无 .cpp：字符串驻留池（**weak root**，字符串专用 set--不进 `mark_roots_`，`collect` 在 sweep 前调 `remove_white()` 摘除指向白色串的表项防 sweep 后悬垂）。

- 裸 `ObjString** slots_` + 低位标签（nullptr 空 / `0x1` 墓碑 / 真指针占用；每槽一个 `ObjString*`），初始槽数 `kInitialCap = 8`，无 ctrl/h2（靠内容比较）；`find`/`insert`/`remove_white`。`find` 含两段重载（先比总长，候选切前/后两个子 view 分别与 lhs/rhs 比较）-- 供拼接方先查后拼（`new_string` 两段重载经 `GC::intern_find` 转发使用）。
- 头循环（GC 持值成员 <-> InternPool 用 `GC*`）经模板延后具现化 + ctor 函数体内 `static_assert` 打破（同 `Object.hpp` 对 GC 的处理）。

## `memory/StringBuilder.hpp`

header-only `aria::StringBuilder`：可增长原始字节缓冲，buffer 经 GC 分配器家族分配（`bytes_allocated_` 自动记账）。构建面 = 带种串构造（`StringBuilder{gc, src}`，gc 构造+append 一体，单发形态消费方直用）+ `append`（直写字节，不持哈希状态；终态 `'\0'` 位由分配口径恒含，交付后 `data_[size_]` 恒为 `'\0'`）+ `reserve`（唯一扩容路径，append 隐式扩容走同口：一律精确按需、无倍增无空余、只扩不缩——超容 append 逐次 realloc 搬迁，碎片化流式追加须先量后填；reserve 的价值=让多发 append 免逐段搬迁，单发 append 自身即精确定容免 reserve）+ `resize`（直写后提交终长：`reserve(n)` 后经 `begin()` 直写 `[begin, begin+n)` 的消费方（解码等外部产源）以此收口，置 `size_` 与终态 `'\0'`、不搬运不清零，前置容量须已定容）；检视面 = `view`/`size`/`begin`/`end`（已建内容直读直改，就地改写无哈希失效协议）；`take_string` 把 buffer 零拷贝移交 `ObjString`（另具 hash 重载，调用方已持终态哈希免重算），内容哈希在铸造口按当时内容一次全算（唯一消费点）。驻留判定收在五参 `new_string` 接管重载：命中零拷贝返已有串 / 长串收缩接管 / 短串 SSO 化释放 buffer。GC 纪律：allocate/reallocate 永不触发 GC（核心不变式），本类全程零 GC 点、无需守卫，唯一 GC 点在 take 的 `new_object` 顶部；take 后空态可复用。

## `memory/RawAlloc.hpp`

`aria::mem::alloc/realloc/free` 三原语：GC 层全部字节流量的后端缝。后端二选一（`ARIA_USE_MIMALLOC` 走 `mi_malloc` 族 / OFF 退 `std::malloc` 族），三口必须同族（new 的块喂 realloc 是 UB）。GC 容器路径（`GC::allocate` 等）与 `ShellPool` 的 span 获取共用此口；非 GC 的编译期裸分配（`memory/AstArena.hpp` 的块获取/释放）亦经此口，分配失败按先例 `fatal_error(OutOfMemory)` 收口、不抛。

## `memory/AstArena.hpp`

header-only AST 专用 bump 分配器（消费方在 compile 层）：节点与列表缓冲自大块（首块 256 KB，放不下按 2 倍翻新块）顺序分配，析构沿块链整批释放、不跑任何析构函数，块链头兼当前填充块（新块恒头插、只从头分配）。构造面 = `make<T>(args...)`（构造语义同 make_unique）+ `make_list(const List<T>&)`（元素一次性拷入 arena，返回 `Span<T>` 视图；空表零分配，元素须平凡可拷贝——static_assert 钉住；它是节点列表字段的唯一生产口，Span 会自容器隐式转换，局部容器不得直接喂字段，别名即悬垂）；检视面 = `node_count()` / `allocated_bytes()`。后备经 `mem::alloc/free`，失败 `fatal_error(OutOfMemory)`、全程不抛。机制契约与实测见 `.claude/reference/memory/ast-arena-notes.md`。

## `memory/ShellPool.hpp`

header-only 对象壳池：按槽尺寸类（8 B 一档，槽尺寸 = sizeof 上取整到 8，≤ `kMaxPooledSlotBytes`=256；超大壳直连后端，正确性不变）的定长空壳仓库，接在 GC 与 RawAlloc 之间。壳只经 `new_object`/`delete_object` 生死，同尺寸死壳就地复用，后端只见 span 大块与容器缓冲流量。

- `alloc<T>()`：`sizeof(T)` 编译期落格（零运行期查表）+ `static_assert(alignof(T) <= 8)` 兜底；`push(shell_bytes, shell)`：按 `Object::size()` 的精确 sizeof 落格，与 alloc 侧同源恒命中同格。
- span = 64 KB 后端大块，块头 `SpanHeader`（链 + bump 游标）切槽；空壳链节点寄生死壳内存头 8 B（析构后死字节，构造时覆写）。
- span 只获取不归还（峰值驻留到进程结束），`~ShellPool` 排空全还后端（~GC `free_all_` 先把残留壳压回池）。**永不触发 GC**；`bytes_allocated_` 逐对象记账留在 `GC::new_object`/`delete_object`，span 开销不计。

## `memory/GC.hpp` / `.cpp`

GC 分配器（`allocate<T>`/`deallocate<T>`/`reallocate<T>` 模板，按 T 计数，**容器/缓冲路径**）+ 对象链表 + `new_object<T>`（壳经 `shell_pool_.alloc<T>` 取槽）/`delete_object`（私有，`new_object` 的逆，壳 `push` 回池，供 `sweep_`/`free_all_` 调）。

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
- `current_` 一点：执行上下文（`ObjMovement : Object`）已入对象链表，其 `trace` 自标值栈 `[base, top)`（run() 期局部/实参/临时值，最关键的根；run() 外为空态）、各活动帧 `closure`（级联标 function/upvalues）与 `module`、挂起错误寄存器、open upvalue 开链节点（「闭包已死而 upvalue 仍在链」的悬垂防线），并经 `mark_object(previous_)` 沿 resume 链级联。

组合而非继承，GC 不识 VM 类型（经 `set_vm_roots` 回调接入）。

**核心不变式**

`allocate<T>`/`reallocate<T>` **永不触发 GC**（壳池 `ShellPool::alloc` 同：取槽/span 获取都不调 `maybe_collect`）；GC 仅在 `new_object` 顶部 `maybe_collect` 与 VM safe point 触发。

这是与「link-on-alloc + publish-after」对象模型绑定的定义性约束，非性能取舍--`new_object` 返回的对象此刻白色无根，需发布进根才安全，而发布动作本身是 trivial 分配；若该分配触发 GC 会扫掉白色无根对象致悬垂。故 `add_constant`/`intern_insert`/`globals().set` 等「fresh 对象跨 trivial 分配再发布」写法免守卫全靠此。`allocate`/`reallocate` 是叶函数，靠契约注释 + review 守；另一方向（裸持白色对象跨真 GC 点漏 `make_guard`）靠显式守卫 + stress GC 测试守。
