# GC 实现计划

aria 解释器的 GC(内存分配 + mark-sweep 回收)设计与分阶段实现路线。本文档记录已确认的设计决策与 Phase 1 的落地细节,后续阶段随实现推进补充。

## 1. 设计目标

- GC 统一管理两类内存:**Object 对象**(带 `Object` 头、可 trace、链入对象链表)与 **trivial 批量存储**(裸字节,由 `Array<T>` 持有,计入 `bytes_allocated_` 但不直接 trace,由 owner 的 `trace()` 间接遍历元素)。
- 提供 **mark-sweep** 三色回收。
- 两层分配都由 GC 控制:对象走 trace/链表;trivial 字节走 `Array<T>`(经 GC 分配器、计数)。
- GC 自身 scratch(gray_stack_ / temp_roots_)用 `List`(std::vector),**不走 Array、不计入 `bytes_allocated_`** -- GC overhead 与 managed heap 分离。
- 分配失败走 `fatal_error(ErrorCode::OutOfMemory, ...)`,与本项目第 4 条错误通道(Resource/Internal 不可恢复)一致。
- GC 不依赖 C++ 异常;只在 `new_object` 顶部与 VM safe point 触发回收(动态类型语言运行时错误可能频繁,C++ 异常栈展开代价不可控)。

## 2. 路线图

| 阶段 | 内容 | 状态 |
| :--- | :--- | :--- |
| **Phase 1** | `Array<T>` + GC(模板分配器 + mark-sweep + 临时根 + `new_object`)+ `ObjString`(SSO,**无驻留**)+ 测试 | 已落地 |
| Phase 2 | `HashTable`(Swiss Table)+ intern 驻留池 + 值绑定容器(`AriaArray`/`AriaHashTable`) | 已落地 |
| Phase 3 | `CodeUnit` + `ObjFunction`/`ObjModule`/`ObjNativeFn`/`ObjException` 已落地;`ObjClosure`/`ObjUpvalue` 已随 M4 闭包落地(2026-09,`ObjFunction` 的捕获描述表 `UpvalueDesc` 同批);`ObjList`/`ObjMap`/`ObjClass`/`ObjInstance`/`ObjBoundMethod` 待后续 | 部分落地 |
| Phase 4 | `Movement`(有栈协程,VM 持 `current_`)+ VM 根(`current_` 单根;协程经对象图可达,M6 定稿不设 `movements_` 并集,见 vm-design.md §4.9)+ safe point | 部分前拉:值栈/帧经 vm_roots tracer 标根 + `JUMP_BACK` safe point 已落地(开发期即开 GC);open upvalue 链亦已随 M4 落地并经 tracer 标根;`ObjMovement : Object` + 协程根收敛仍待 M6 |

> intern 延后到 Phase 2:它依赖 HashTable,而 HashTable 是 Phase 1 之后的下一个产物(与 Array 平级、并列的通用容器,不依赖 Array)。Phase 1 不引入 `std::unordered_map` 占位代码,GC 核心(分配计数 / mark-sweep / 临时根 / ObjString 析构)已可独立测试。

## 3. Phase 1 详细(已落地)

### 3.1 文件清单

| 文件 | 说明 |
| :--- | :--- |
| `src/memory/Array.hpp` | `template<typename T, typename Alloc = GC> class Array`,header-only,建在 `Buffer<T,Alloc>` 上、经分配器自释放 |
| `src/memory/GC.hpp` | GC 类:模板分配器 + 对象链表 + `new_object` 模板 + mark-sweep + 临时根 + `Guard` |
| `src/memory/GC.cpp` | GC 非模板方法实现(ctor/dtor/collect/sweep_/free_all_/delete_object/push_temp_root...) |
| `src/object/Object.hpp` | 加纯虚 `trace(GC&)` / `size()`;地址哈希经 `util::hash_addr`(`util/util.hpp`) + 两 ctor;前向声明 `GC` |
| `src/object/ObjString.hpp` / `.cpp` | SSO 字符串 + FNV-1a 哈希 + `new_string` 工厂 |
| `CMakeLists.txt` | 登记新文件 + `ARIA_DEBUG_GC` 选项 |
| `tests/memory/test_array.cpp` / `test_gc.cpp` | 单测 |

### 3.2 头文件依赖(单向,无环)

```
GC.hpp -> object/Object.hpp -> value/Value.hpp
               |
               +-> util/io.hpp, util/util.hpp
GC.hpp 还 include error/Error.hpp + <format>/<cstring>/<algorithm>(模板分配器内联体用)
Array.hpp -> memory/Buffer.hpp + memory/Allocator.hpp(经分配器解耦,不 include GC.hpp)
```

`GC.hpp` **不** include `Array.hpp`:GC 的 scratch 用 `List`,不走 Array。`Array.hpp` 也不 include `GC.hpp`--经 `TrivialAllocator` concept 与具体分配器解耦,使用 GC 的具体类(`AriaArray`/`CodeUnit` 等)自行 include `GC.hpp`,无循环。

> 历史:曾让 `gray_stack_`/`temp_roots_` 用 `Array<T>` 自举,但 GC 需 Array 成员 -> `GC.hpp` include `Array.hpp` -> Array 内联体 `gc_->xxx` 需 GC 完整 -> 循环,被迫拆 `Array.impl.hpp`。后改用 `List` 解掉,更干净(GC overhead 与 managed heap 分离),`Array.impl.hpp` 删除。

### 3.3 核心数据结构与 API

#### `Array<T>`(`memory/Array.hpp`)

基于 GC 分配器的可扩容 trivial 数组(**顺序**增长)。`T` 须 `TriviallyCopyable`(`Value`/`OpCode`/`u8`/`i32` 等 POD),分配器须 `TrivialAllocator`(默认 `GC`)。建在 `Buffer<T,Alloc>`(收口分配/重分配/释放)之上,加 `usize len_` 逻辑长度。不可拷贝/不可移动。扩容固定初始 8、2 倍几何增长(内联,无策略模板参数)。

```cpp
template<TriviallyCopyable T, TrivialAllocator Alloc = GC>
class Array {
    Buffer<T, Alloc> buf_;   // 收口 allocate/reallocate/deallocate,无逻辑长度
    usize len_ = 0;          // 逻辑长度(<= cap)
public:
    explicit Array(Alloc* alloc) noexcept;
    void push(const T& v);                   // 容量不足 2x 扩容
    void reserve(usize n);                   // buf_.reserve -> alloc->reallocate<T>(memcpy 旧块)
    void resize(usize n, T fill = T{});       // 注意 Value{} 是 f64 0.0 非 nil
    void truncate(usize n) noexcept;          // 不释放容量
    void clear() noexcept;                    // len_ = 0,不释放容量
    void pop() noexcept; T& top() noexcept;
    T& operator[](usize i) noexcept;
    T* data() noexcept; Span<T> span() noexcept;
    usize size() / capacity() const noexcept;
    bool empty() const noexcept;
};
```

用途:**顺序**增长的可扩容数组(ObjList 元素 / CodeUnit 字节码与常量池)。扩容走 `reallocate<T>`(memcpy 旧数据到新块),不触发 GC。

> **不适合 HashTable**:HashTable 的 rehash 要按新容量重算每个元素位置,memcpy 会放错。HashTable 直接用 `GC::allocate<Bucket>` / `deallocate<Bucket>` 自管 bucket 数组(一次性分配,达阈值后重新分配 + rehash 全部元素),不走 Array。intern 驻留池同理。

#### `GC`(`memory/GC.hpp`)

```cpp
class GC {
public:
    GC() noexcept; ~GC();
    // ---- 类型化 trivial 分配(模板,内联在 GC.hpp)----
    template<typename T> [[nodiscard]] T* allocate(usize count);          // 失败 fatal_error(OutOfMemory)
    template<typename T> void deallocate(T* p, usize count) noexcept;
    template<typename T> [[nodiscard]] T* reallocate(T* p, usize old_count, usize new_count);
    // ---- object 分配 ----
    template<DerivedFromObj T, typename... Args> [[nodiscard]] T* new_object(Args&&... args);
    // ---- trace ----
    void mark_value(Value v) noexcept;
    void mark_object(Object* o) noexcept;
    void maybe_collect() noexcept;                // bytes_allocated_ >= next_gc_ 或 stress 时 collect
    void collect();                               // mark_roots_ -> trace_gray_ -> sweep_
    // ---- 临时根 ----
    // push_temp_root/pop_temp_root 为私有(仅 Guard 内部调用);公共增量压入 API 为 Guard::push
    class Guard { /* RAII,禁拷贝/移动,经 make_guard 的 prvalue 必然复制消除 */
                  void push(Value v) noexcept;
                  void push(Object* o) noexcept; };
    Guard make_guard() / make_guard(Value) / make_guard(Object*);
    usize bytes_allocated() const noexcept;
    void set_stress(bool) noexcept;               // 运行期压力开关(测试用)
    // ---- GC 禁用锁(单线程,计数器,支持嵌套)----
    void disable_gc() noexcept;                   // ++lock_count_
    void enable_gc() noexcept;                    // --lock_count_(assert >0)
    bool is_gc_disabled() const noexcept;         // lock_count_ > 0
    class LockGuard { /* RAII:构造 disable,析构 enable */ };
    LockGuard make_lock() noexcept;
    // ---- VM 根 tracer(后补,见 Phase 4)----
    void set_vm_roots(std::function<void(GC&)> tracer) noexcept;
    // ---- intern 委托(后补,见 Phase 2)----
    ObjString* intern_find(StringView s) noexcept;   // 委托 intern_
    ObjString* intern_insert(StringView s);          // 委托 intern_
private:
    Object*       objects_head_;
    usize         bytes_allocated_;
    usize         next_gc_;                       // 初始 4096,每次 collect 后 *= 2
    bool          is_stress_;
    u32           lock_count_;                    // GC 禁用计数(>0 禁用,支持嵌套)
    List<Object*> gray_stack_;                    // GC scratch,不计入 bytes_allocated_
    List<Value>   temp_roots_;                    // GC scratch,不计入 bytes_allocated_
    InternPool    intern_;                        // 字符串驻留池(Phase 2)
    std::function<void(GC&)> vm_roots_tracer_;    // VM 根回调(Phase 4 前拉)
};
```

**关键实现点:**

- 分配器三方法是**模板** `allocate<T>(count)` / `deallocate<T>(p, count)` / `reallocate<T>(p, old, new)`,按 T 元素计数(内部 `count * sizeof(T)`)。模板体 inline 在 `GC.hpp`,故 GC.hpp 需 include `error/Error.hpp` + `<cstring>`/`<algorithm>`(模板体用到 `fatal_error` / `std::memcpy` / `std::min`;fatal_error 自带格式化,调用点不直接用 `<format>`)。
- `new_object` 用 `allocate<u8>(sizeof(T))` 取裸内存再 placement-new,链入 `objects_head_`。
- `delete_object(Object*)` 是 `new_object` 的逆:`size()`(虚,须在 `~Object` 前)-> `~Object()`(级联释放子内存:Array / long_chars_)-> `deallocate<u8>`(壳)。**不含链表摘除**(由 `sweep_`/`free_all_` 调用方管),`sweep_` 与 `free_all_` 共用此函数,销毁逻辑收口一处。
- **不变式**:`allocate` / `reallocate` 永不触发 GC,故对象构造期内的子分配不会回收正在构造的对象(其尚未链入/未标根)。GC 仅在 `new_object` 顶部(`maybe_collect`)与 VM safe point 触发。
- `new_object` 在分配**之前** `maybe_collect()`,新对象尚不存在,无虞。
- 成员声明顺序:基本类型在前,`List`(scratch)/`InternPool`/tracer 在后。成员析构逆序 -> scratch `List` 先析构(其 dtor 不碰 `bytes_allocated_`,无依赖)。
- `~GC` 调 `free_all_()` 释放所有残留对象。
- **GC 禁用锁**:`u32 lock_count_` 计数器(单线程,非 mutex--无竞争且 mutex 非递归会死锁;非裸 bool--不支持嵌套)。`disable_gc`/`enable_gc` 增减计数,`>0` 即禁用;嵌套自然支持。检查点在 `collect()`(单一咽喉):`maybe_collect` 与显式 `collect()` 都经此,锁住时均跳过。RAII `LockGuard`/`make_lock()` 保证 disable/enable 配对(临界区异常/提前返回也能恢复)。用于临界区(如 finalizer 执行、堆结构变动中)禁止回收。

#### `Object` 改动(`object/Object.hpp`)

```cpp
class GC;   // 前向声明(trace 形参用)

// 指针地址哈希(对象身份哈希):util::hash_addr,定义于 util/util.hpp(Vigna lowbias32),Object.hpp include 复用。

class Object {
public:
    Object() = delete;
    // 内容哈希型(ObjString/ObjRange 等不可变对象):显式传算好的内容哈希。
    Object(u32 hash, ObjType type) noexcept;
    // 地址哈希型(可变对象默认):hash_ = util::hash_addr(this)。
    Object(ObjType type) noexcept;
    // ... type() / is_marked() / mark() / unmark() / is<T> / as<T> / 成员 next_ hash_ type_ is_marked_ ...
    [[nodiscard]] u32 hash() const noexcept;              // 返回缓存哈希(内容或地址,构造时定)
    virtual void  trace(GC& gc) const noexcept = 0;       // 标记子节点
    [[nodiscard]] virtual usize size() const noexcept = 0; // 真实分配字节数,sweep 释放壳用
    virtual ~Object() = default;                           // 虚析构:级联释放子内存
};
```

**hash 设计**:`hash_` 保留在 Object,语义为「本对象的缓存哈希」(不可变=内容、可变=地址),构造时一次性算好。`object_hash(o)` 退化为 `return o->hash()`,无需类型分发--分发在**构造期**(子类 ctor 选内容或地址),不在查询期。

- 内容哈希型(`ObjString`/`ObjRange`):`Object{content_hash, Kind}`。ObjString 现状已是(FNV-1a)。
- 地址哈希型(可变对象):`Object{Kind}`(内部 `hash_ = address_hash(this)`)。`this` 在 ctor init list 仅取地址,合法。
- 地址哈希:`util::hash_addr`(`util/util.hpp`,Vigna lowbias32);对象 16 字节对齐(低位冗余),经混合分散。

> 为何不把 `hash_` 移出 Object、改用 `object_hash(o)` 按类型分发?实测:去掉 `Object::hash_` 后 Object 仍 24(对齐填充吃掉 4 字节,没省),而 ObjString 需自带 `hash_` -> 56 涨到 64(多态基类尾部 padding 不被派生类复用)。移出是 ObjString 的内存回退(+8/串),故保留 `hash_` 于 Object。

**无 `destroy` 钩子**。子内存释放统一走虚析构:
- `Array` 成员:靠 `~Array` 自释放(持 `GC*`)。
- 非 Array 子内存(如 `ObjString` 的 `long_chars_`):在子类 `~dtor` 里经自己持的 `GC*` 释放。

sweep 顺序(封装为 `delete_object`):`size()` -> `~Object()`(级联释放子内存)-> `deallocate<u8>(壳)`。

> 历史:曾有 `virtual void destroy(GC&)` 钩子(sweep 在 `~Object` 前调用,释放非 Array 子内存),因为析构函数拿不到 GC 引用。后改为:子类自己持 `GC* gc_`,析构时直接释放。去掉 destroy,sweep 更简单,且 `ObjString` 的 `gc_` 填掉原 `is_long_` 的填充,`sizeof` 不增。

#### `ObjString`(`object/ObjString.hpp`)

SSO 字符串:

```cpp
class ObjString : public Object {
public:
    static constexpr usize kShortCapacity = 15;
private:
    GC* gc_;                                       // 供 ~ObjString 释放 long_chars_
    union {
        char  short_chars_[kShortCapacity + 1];   // 16 字节,短串内联(15 字符 + NUL)
        char* long_chars_;                         // 长串独立 buffer
    };
    usize length_;
public:
    ObjString(GC& gc, StringView src);
    ~ObjString() override;                         // if (is_long()) gc_->deallocate<char>(long_chars_, length_+1)
    StringView view() const noexcept;
    usize length() const noexcept;
    bool is_long() const noexcept { return length_ > kShortCapacity; }  // 派生,不存标志位
    void trace(GC&) const noexcept override {}     // 纯字节,无子节点
    usize size() const noexcept override { return sizeof(ObjString); }
};

ObjString* new_string(GC& gc, StringView src);    // = gc.new_object<ObjString>(gc, src)
```

- `length_ <= 15`:内联 `short_chars_`,无额外分配。
- `length_ > 15`:`long_chars_ = gc.allocate<char>(length_+1)`,独立 buffer。
- `is_long()` 由 `length_ > kShortCapacity` **派生**(不存标志位,省 1 字节 + 填充)。
- `gc_` 正好填掉原 `is_long_` 浪费的填充,`sizeof(ObjString)` 保持 **56**(DEBUG 日志确认)。
- 哈希:FNV-1a 32-bit,构造时算出存 `Object::hash_`。
- `trace()` 空(纯字节无子节点);`~ObjString` 释放 `long_chars_`。
- Phase 1 无驻留:`new_string` 直接 `new_object<ObjString>`。Phase 2 接 intern 驻留池。

### 3.4 mark-sweep 算法

```
collect():
  mark_roots_()    // temp_roots_ + vm_roots_tracer_(modules_ + builtins_ + object_class_(M5 第 4 根) + current_ 沿 previous_ 执行链各上下文值栈/帧/挂起错误寄存器)
  trace_gray_()    // gray 栈弹一个 -> o->trace(*this) -> 子节点标灰入栈
  sweep_()         // 遍历 objects_head_:未标 -> 摘除 + delete_object();已标 -> unmark()
  next_gc_ = bytes_allocated_ * 2
```

- `mark_object(o)`:已标则跳过;否则 `o->mark()` + `gray_stack_.push_back(o)`。
- `sweep_` 用 `Object** slot` 双指针摘除节点,O(n) 单趟。
- `size()` 必须在 `~Object()` 之前调用(虚调用,析构后 vtable 失效)。

### 3.5 临时根与 Guard

C++ 局部变量持有的、尚未入值栈的对象/值,在分配序列间保护其不被回收:

```cpp
auto guard = gc.make_guard(s);       // push s
(void) new_string(gc, "trigger");   // 触发 GC:s 被标根 -> 存活
// guard 析构时 pop
```

`push_temp_root` 有 `Value` / `Object*` 双重载,内部统一 `List<Value>`。`Guard` RAII,禁拷贝/移动(`make_guard` 返回 prvalue,必然复制消除)。

### 3.6 测试

`tests/test_array.cpp`(7 个):push/pop/index、几何扩容、reserve/resize、truncate/clear、move 语义、空 dtor、`Array<Value>`。

`tests/test_gc.cpp`(17 个):
- `GcAlloc`:BytesCounted / FreeNullIsNoop / ReallocCopiesAndAdjusts / ReallocToZeroFrees
- `ObjString`:ShortIsInline / LongIsSeparate / HashStableForEqualContent
- `GcCollect`:EmptyCollectIsNoop / UnrootedShortSwept / UnrootedLongSwept / TempRootSurvives / SweepResetsMarks / GuardBalancesTempRoots
- `Object`:AddressHashCtor(地址哈希 ctor)
- `GcLock`:DisablePreventsCollect / ExplicitCollectRespectsLock / NestingAndGuard

> `UnrootedShortSwept` 用显式 `gc.collect()`(而非 stress 触发):短串壳(56B)回收与 trigger 短串(56B)新增净变化为 0,stress 下无法用字节数判定;显式 collect 无新分配,严格下降。

### 3.7 CMake

```cmake
# aria_core 源列表加:src/memory/Array.hpp, src/memory/GC.{hpp,cpp},
#                    src/object/ObjString.{hpp,cpp}

option(ARIA_DEBUG_GC "Enable GC debug logging" OFF)
if (ARIA_DEBUG_GC)
    target_compile_definitions(aria_core PUBLIC DEBUG_LOG_GC)
endif()
```

`DEBUG_LOG_GC` 宏被 `log_obj_alloc`(Object.hpp)与 `GC::collect` 的日志使用。

> 注意:`Object.hpp` 的 `log_obj_alloc` 用 `to_void_ptr`(`util/util.hpp`),曾漏 include,只在 `ARIA_DEBUG_GC=ON` 时暴露,已修(补 `#include "util/util.hpp"`)。

## 4. Phase 1 验证状态

- `clang++ -std=c++23 -I src -fsyntax-only` 逐文件通过
- `aria_core` / `aria_tests` / `aria` 全部构建通过(MinGW Makefiles + clang)
- `ctest` 264/264 全绿(含 24 个新测试:7 Array + 17 GC)
- `ARIA_DEBUG_GC=ON` 构建通过,日志输出正常(如 `gc end: 99 -> 0` 确认 `~ObjString` 释放 long_chars_)
- `clang-format -i` 收尾(LLVM / 4 空格 / 120 列)

## 5. 后续阶段

### Phase 2:HashTable(Swiss Table)+ intern 驻留池 + 值绑定容器(已落地)

> **落地偏差(相对下文设计描述)**:
> 1. **trace 放 AriaHashTable,非 HashTable**:下文 HashTable 段画了 `trace`(直访 `ctrl_`/`entries_`)。实际为守住「`src/memory/` 值无关」分层,`HashTable` **不** include `Value.hpp`、无 `trace`;改提供 public `for_each_occupied(Fn&&)`,`AriaHashTable::trace` 经它调 `mark_value`。`AriaArray::trace` 同理(直接遍历 `Array<Value>`)。
> 2. **`upsert` 替代 find+insert**:下文示意「调用方先 `find` 查重,未命中才 `insert`」。实际 `HashTable` 提供 `Entry* upsert(const K&)`(find-or-insert,命中返回已有 Entry 保留其 value,未命中插入 `value=V{}`),消除「必须先 find」前置条件与重复插入风险;另提供只读 `find` 与 `erase(key)`。
> 3. **ValueHash/ValueEq provisional 落地**:下文说这两个 functor「随 ObjMap 落地」(Phase 3)。但 Phase 2 要让 `AriaHashTable` 可编译可测,故提供 provisional 版(基于两表示共有的 `type()`/`as_*()`,**不**用 NanBoxing 专属的 `bits()`/`same_bits()`--TagValue 未提供,故两表示都编译):Obj 用 `as_obj()->hash()`(ObjString 即内容 FNV-1a)/ 指针相等(intern 后等价内容同指针)。provisional 点(int 1 vs f64 1.0 不同键、f64 NaN 未规范化)留 Phase 3 随 ObjMap 精化。**值操作收口于 Value 层**:`value_hash`/`value_equal` 自由函数声明在 `value/Value.hpp`、定义在 `value/Value.cpp`(`<bit>`/`Object.hpp` 依赖置于 `.cpp`,不污染被广泛 include 的 `Value.hpp`);`ValueHash`/`ValueEq` 为 `AriaHashTable.hpp` 内的内联包装(仅 AriaHashTable 用,转发到自由函数)。
> 4. **GC↔InternPool 头循环**:GC 持 `InternPool` 值成员(GC.hpp 需 InternPool 完整),InternPool 方法用 `gc_->allocate`(需 GC 完整)。现行解法:InternPool 为 header-only 模板 `InternPool<Alloc = GC>`,头循环靠模板延后具现化 + ctor 函数体内 `static_assert` 打破(同 `Object.hpp` 对 GC 的处理)。
> 5. **GC 暴露 `intern_find`/`intern_insert`** 委托 `intern_`(保持 private),`new_string` 经此驻留。`collect` 在 `trace_gray_` 后、`sweep_` 前调 `intern_.remove_white()`。
> 6. **`find` 内部算哈希**:下文设计 `find(const K&, u32 hash)` 收哈希参数;实际 `find(const K&)`/`upsert` 内部调 HashFunctor 算哈希,调用方免传。
> 7. **判满公式**:`(count_ + tombstones_ + 1) * 8 > cap_ * 7`(含本次插入),非下文示意 `count_ + tombstones_ > cap_ * 7/8`。

#### 文件位置与分层

**`src/memory/`:值无关的通用容器**(不依赖 Value)

- `Array.hpp`:`Array<T>`(顺序可扩容,Phase 1 已落地)。
- `HashTable.hpp`:通用 Swiss Table 模板 `HashTable<K, V, Hash, Eq>`。
- `InternPool.hpp`(或 `StringPool.hpp`):intern 驻留池,GC 持有。

**`src/value/`:绑定 Value 的 aria 容器**(在通用容器之上加 `trace(GC&)` + aria 语义)

- `AriaArray.hpp`:`AriaArray : public Array<Value>`--继承 Array<Value> 的存储与接口(push/[]/span...),加 `trace(GC&)`(遍历元素 `mark_value`)与 aria 专属操作。
- `AriaHashTable.hpp`:`AriaHashTable : public HashTable<Value, Value, ValueHash, ValueEq>`--继承 HashTable 的 Swiss Table 实现与 `trace`(`ValueHash`/`ValueEq` 由运行时按 aria 的值相等语义提供,随 ObjMap 落地)。

> 分层原则:`src/memory/` 的容器对 T/K/V 完全通用(不知道 Value 为何物);`src/value/` 的容器把模板实参绑成 `Value` 并补上 GC trace 与 aria 语义。Phase 3 的 `ObjList`/`ObjMap`(Object 子类型)持 `AriaArray`/`AriaHashTable` 作成员。

> 继承而非组合:让 `AriaArray`/`AriaHashTable` 直接复用底层容器的全部公开接口,免去转发样板。底层容器 dtor 非虚--这两个子类不作为多态基类使用(不会拿 `Array<Value>*` 指向 `AriaArray` 再 delete),故安全;若子类新增资源持有成员,各自补 dtor 即可。

HashTable 与 InternPool 都**不走 Array**:一次性分配、达阈值重新分配 + rehash 全部元素,与 Array 的顺序 push + memcpy 扩容不匹配(rehash 要按新容量重算位置,memcpy 会放错)。直接用 `GC::allocate<T>(cap)` / `deallocate<T>(p, cap)` 自管数组。

#### HashTable:Swiss Table 标量版

**核心思路**:与「方案 A(独立 state 数组)」同开销(1 字节/槽的 ctrl),但 ctrl 字节同时编码「占用槽的 7 位部分哈希(h2)」。探测时先比 h2,不命中就跳过且**不加载 Entry(16B)**,只在 h2 命中时才加载 Entry 比全键。

**ctrl 字节编码**:

```cpp
namespace ctrl {
    inline constexpr u8 kEmpty   = 0xFF;  // 空槽(探针终止)
    inline constexpr u8 kDeleted = 0xFE;  // 墓碑
    // 占用:0x00..0x7F,高位 0,低 7 位 = h2(部分哈希)
    [[nodiscard]] inline constexpr bool is_occupied(u8 c) noexcept { return (c & 0x80) == 0; }
    [[nodiscard]] inline constexpr bool is_empty(u8 c)     noexcept { return c == kEmpty; }
    [[nodiscard]] inline constexpr u8   from_h2(u32 h2)    noexcept { return static_cast<u8>(h2 & 0x7F); }
}
```

高位 1 = 特殊(空/墓碑),高位 0 = 占用 + h2。`kEmpty`/`kDeleted` 高位都是 1,故 `c == target`(target = h2 字节,高位 0)只会命中占用槽,不会误中特殊值。

**h1/h2 拆分 + 2 的幂 cap + 三角探测**:

```cpp
[[nodiscard]] inline constexpr u32 h1(u32 hash) noexcept { return hash >> 7; }   // 高 25 位 -> 槽索引
[[nodiscard]] inline constexpr u32 h2(u32 hash) noexcept { return hash & 0x7F; } // 低 7 位 -> ctrl
// cap_ 为 2 的幂:槽索引 = h1(hash) & (cap_ - 1)
// 三角探测:pos = start; step = 1; 每次 pos = (pos + step) & mask; ++step;  偏移序列 1,3,6,10,...
```

**模板与成员**:

```cpp
template<typename K, typename V, typename Hash, typename Eq>
class HashTable {
    struct Entry { Value key; Value value; };  // 16B(K/V 实例化成 Value)
    Entry* entries_;   // gc_->allocate<Entry>(cap_)
    u8*    ctrl_;      // gc_->allocate<u8>(cap_)
    GC*    gc_;
    usize  cap_;       // 2 的幂
    usize  count_;     // 占用数
    usize  tombstones_;
    // 不可拷贝/不可移动,持 GC*,同 Array
};
```

- 两个分配(ctrl_ + entries_),rehash 时一起重分配、重插、释放旧两个。
- **不存全哈希**:rehash 时对每个占用槽 `Hash{}(key)` 重算(ObjString* 键读 `hash_`,int 键重算,都廉价;rehash 本就稀有)。

**lookup(核心收益)**:探针走 ctrl_(1B/槽,8 槽/缓存行),绝大多数不命中只读 1 字节、不碰 16B Entry。

```cpp
const Entry* find(const K& key, u32 hash) const noexcept {
    if (cap_ == 0) return nullptr;
    const usize mask   = cap_ - 1;
    const u8    target = ctrl::from_h2(h2(hash));
    usize pos = h1(hash) & mask;
    usize step = 1;
    while (true) {
        const u8 c = ctrl_[pos];
        if (c == target) {                          // h2 命中 -> 才加载 Entry 比全键
            if (Eq{}(entries_[pos].key, key)) return &entries_[pos];
        } else if (ctrl::is_empty(c)) {
            return nullptr;                         // 空槽,探针终止
        }
        pos = (pos + step) & mask;
        ++step;
    }
}
```

**insert / erase / rehash**:

```cpp
// erase:ctrl 置墓碑。无需 nil-out entries_,trace 按 ctrl 跳过非占用槽(见下)。
void erase(usize i) noexcept {
    ctrl_[i] = ctrl::kDeleted;
    --count_;
    ++tombstones_;
}
```

- insert:命中墓碑记住首个位置,探到空槽时回退到首个墓碑写入(复用墓碑则 `--tombstones_`)。调用方先 `find` 查重,未命中才 insert。
- rehash 触发:`count_ + tombstones_ > cap_ * 7/8`(Swiss Table 常用 7/8 负载因子),或 `tombstones_ > cap_ / 8`(墓碑过多拖慢查找)。rehash:重分配 ctrl_(全 `kEmpty`)+ entries_,逐个占用槽重算 hash 重插,释放旧两块;`tombstones_` 清零。

**trace**:只看 ctrl,不加载非占用 Entry,故空/墓碑槽里是垃圾也安全。

```cpp
void trace(GC& gc) const noexcept {
    for (usize i = 0; i < cap_; ++i) {
        if (ctrl::is_occupied(ctrl_[i])) {
            gc.mark_value(entries_[i].key);
            gc.mark_value(entries_[i].value);
        }
    }
}
```

**SIMD(未来 perf pass,不急)**:标量版已吃到 h2 的主要红利(跳过 Entry 加载)。再压榨可用 SSE2/NEON 一次比 16 个 ctrl 字节,并行找「h2 命中」与「遇到 kEmpty」。为此需把 `ctrl_` 末尾补 `SIMD_WIDTH - 1` 字节镜像(开头内容的副本),让末尾附近的无边界 16 字节读取不越界。平台分流走 `src/sys.hpp` 的 `SYS_WINDOWS`/`SYS_LINUX`/`SYS_MACOS`(x64 SSE2、ARM NEON)。

#### intern 驻留池:裸 `ObjString*` + 低位标签

驻留池是**字符串专用**(键即串内容,值即 `ObjString*` 自身,是 set 而非 map),用比通用 HashTable 更省的特化表示:

```cpp
// src/memory/InternPool.hpp
class InternPool {
    ObjString** slots_;   // gc_->allocate<ObjString*>(cap_)
    GC*   gc_;
    usize cap_;           // 2 的幂
    usize count_;
    usize tombstones_;
    // 不可拷贝/不可移动,持 GC*
};
```

**低位标签**(对象指针经 `::operator new` 是 max_align_t 对齐,低 4 位全 0,`0x1`..`0xf` 空闲):

- `nullptr` (0x0) = 空槽
- `(ObjString*)0x1` = 墓碑
- 真 `ObjString*`(低位 0)= 占用

8 字节/槽,无 ctrl、无 h2(靠内容比较)。lookup:hash 串内容 -> 探针 -> 占用槽比 `view() == query`,墓碑跳过,空槽终止。insert:首个墓碑或空槽写入。

> 若将来 intern miss 频繁想加 h2 过滤,可升级成 Swiss-set(`ObjString*[]` + `u8[] ctrl_`,9B/槽,用 `hash_` 的 h2),当前不必。

#### GC 集成

- `GC` 持 `InternPool intern_` 成员。intern_ 的 slots_ 经 `gc_->allocate<ObjString*>` 分配,**计入 `bytes_allocated_`**(同 Array 的存储);InternPool 本身是 GC 的普通成员(非 Object、不被 trace、不被 sweep),其 slots_ 在 rehash 或 `~InternPool` 时释放。
- `new_string(GC& gc, StringView src)` 改为:
  1. `intern_.find(src)` 命中 -> 返回已有串(不分配、不 GC)。
  2. 未命中:`new_object<ObjString>` 分配 + `intern_.insert(s)`。`new_object` 顶部 `maybe_collect` 在分配前完成,新串尚不存在无虞;ctor 内 `allocate<char>` 不触发 GC;insert 的 rehash 走 `reallocate` 也不触发 GC。用 `Guard` 临时根保护新串直至 insert 完成(防御性)。
- **驻留池是 weak root**(不进 `mark_roots_`),否则驻留串永生。sweep 前 `intern_.remove_white()` 遍历 slots_,摘除指向白色(未标)`ObjString*` 的表项,避免悬垂;随后 sweep 释放这些串。
- 字面量由 CodeUnit 常量池持有(经 `ObjFunction::trace` 标记),驻留使运行时 `==` 串与字面量共享。

#### Phase 2 验证状态

- `clang++ -std=c++23 -I src -fsyntax-only` 逐文件通过
- `aria_core` / `aria_tests` / `aria` 全部构建通过(MinGW Makefiles + clang)
- `ctest` 293/293 全绿(原 264 + 新增 29:12 HashTable + 7 InternPool + 10 Aria 容器)
- `ARIA_DEBUG_GC=ON` 构建通过,日志确认 intern/remove_white 行为(如 `gc end: 155 -> 64` 确认无根驻留串壳+buffer 被回收、再 make 触发新分配)
- `clang-format -i` 收尾

##### Phase 2 文件清单

| 文件 | 说明 |
| :--- | :--- |
| `src/memory/HashTable.hpp` | 通用 Swiss Table 模板 `HashTable<K,V,Hash,Eq>`(header-only,值无关,`upsert`/`find`/`erase`/`for_each_occupied`) |
| `src/memory/InternPool.hpp` | 字符串驻留池(低位标签,weak root;header-only 模板,头循环经模板延后具现化 + ctor `static_assert` 打破) |
| `src/value/AriaArray.hpp` | `AriaArray : public Array<Value>` + `trace` |
| `src/value/AriaHashTable.hpp` | `AriaHashTable : public HashTable<Value,Value,ValueHash,ValueEq>` + `trace`;含 `ValueHash`/`ValueEq` 内联包装(转发到 value_hash/value_identical,哈希键用 ===) |
| `src/value/Value.hpp` / `.cpp` | 值操作收口:`value_hash`/`value_equal` 自由函数(声明在 .hpp,定义在 .cpp;`<bit>`/`Object.hpp` 置于 .cpp) |
| `src/memory/GC.{hpp,cpp}` | 加 `intern_` 成员 + `intern_find`/`intern_insert` + `collect` 调 `remove_white` |
| `src/object/ObjString.{hpp,cpp}` | `new_string` 改驻留(find -> new_object + Guard + insert) |
| `tests/test_hashtable.cpp` / `test_internpool.cpp` / `test_aria_containers.cpp` | 单测 |

### Phase 3：CodeUnit + Object 子类型

- `CodeUnit`：内含 `Array<u8> code`(字节流) + `AriaArray constants`(常量池) + `Array<LineEntry> lines`(RLE 行号表) + `Array<TryRecord> try_records`(异常记录表)。
- `ObjFunction`：trace name(string)+ codeunit 常量池；不 trace 字节码。
- `ObjList`:持 `AriaArray` 成员;trace 委托 `AriaArray::trace`。
- `ObjMap`:持 `AriaHashTable` 成员;trace 委托 `AriaHashTable::trace`。
- `ObjClosure` / `ObjUpvalue`(已落地,M4 闭包 2026-09:`ObjClosure` 持 function + `Array<ObjUpvalue*>` 逐个 trace;`ObjUpvalue` open/closed 双态,trace 标 `*value_slot()`)/ `ObjClass` / `ObjInstance` / `ObjBoundMethod`:各自的 `trace()`。
- 工厂函数 `new_xxx(GC&, ...)` per type,定义在对应 `ObjXxx.hpp`。

### Phase 4:Movement + VM 根

> **已前拉部分(开发期即启用 GC)**:`modules_` + `builtins_` + `object_class_`(M5 tracer 第 4 根) + `current_` 沿 `previous_` 执行链各上下文的值栈 `[base, top)`/各活动帧 `closure`/`module`/挂起错误寄存器/open upvalue 开链(M4 起一并标)已经 `AriaVM` 的 vm_roots tracer 在 `mark_roots_` 标根(Movement 仍是纯 C++ 类,以 tracer 直标代替升 Object);`run()`/`compile()` 不再持 `LockGuard`,`JUMP_BACK` + `new_object` 内已是 safe point;`compile()` 以 `make_guard(&module)` 根化建设中的 `ObjFunction`/常量池链,`CodeGen` 各 `new_string` name 串跨子编译均 `make_guard`。集成测试开 stress GC 主动锻炼。仍待 M6 的部分(下方)为:`ObjMovement : Object` 化、协程根收敛为 `current_` 单根(M6 定稿,不设 `movements_` 并集,见 vm-design.md §4.9)、`CALL`/协程切换 safe point(open upvalue 链已随 M4 落地于 Movement、经 vm_roots tracer 标根,不再待 M6)。

- `Movement`(协程单元,作 Object 子类型):持 `Array<Value> value_stack_`、`FrameStack<CallFrame> frames_`、`ObjUpvalue* open_upvalues_`、`MovementState`。`trace()` 遍历值栈/帧/upvalue/`previous_`/挂起错误寄存器(对标 Wren `blackenFiber`)。
- VM 持 `Movement* current_`(唯一 VM 级协程根)。`mark_roots_` 保留 `current_ -> previous_` 链遍历直标(main_ctx_ 不入堆、非对象,运行中协程的 `previous_` 指向它时对象图不可达,只能链遍历覆盖);挂起协程因 yield/完成解链(`previous_` 恒空)经用户持有的协程值走对象图(M6 定稿,取代早期「`movements_` 列表并集标根」方案,见 vm-design.md §4.9)。
- safe point:`CALL`、循环回边、`new_object` 内、协程切换点(yield/resume)。
- mark 成本 = O(所有活协程栈深之和);协程多时考虑增量标记(未来)。
- 任何跨分配持有的 Value 必须走 `Guard` 临时根(C 栈对 GC 不透明)。

## 6. 已知坑

- **NanBoxing 下 `Value{}` 是 f64 0.0 非 nil**:`Array<Value>` 留空槽当 nil 要显式 `Value::nil_val()`。
- **`SourceFile` 指针稳定性**:`ObjString` 若缓存 `SourceLoc` 之类,注意 `SourceFile` 不得在 String 存活期被 move(SSO 短串 move 后地址变)。
- **`Array<T>` 不 trivial**(有 `gc_` 指针、不可拷贝/不可移动),不能进 `FrameStack`(要求 trivially-copyable)。设计如此:Array 是堆背书可扩容,`FrameStack` 是定容槽位池。
- **`new_object` 模板内 `allocate<u8>(sizeof(T))`**:`u8` 是字节级用法,T 由外层 `new_object<T>` 决定。
- **`is_long()` 派生而非存储**:`ObjString` 不可变(length_ 构造后不变),派生安全。若有可变长度的未来子类型,需重新评估。
- **InternPool 是 weak root,不钉住驻留串**:驻留串的生命周期由「真实根」决定(值栈/globals 等),驻留池只去重不保活。无根的驻留串会在下次 collect 经 `remove_white` 摘表项 + sweep 释放。**故 `new_string` 返回的裸 `ObjString*` 跨任何分配序列必须自行根化(Guard/入值栈)**,否则可能被回收成悬垂(与 Phase 1 同一纪律,intern 不改此)。
- **`AriaArray`/`AriaHashTable` 非 Object,GC 不自动 trace 它们**:它们是 C++ 类(继承 `Array<Value>`/`HashTable<...>`),不链入对象链表、不进 `mark_roots_`。其 `trace(GC&)` 须由 owner(Phase 3 的 `ObjList`/`ObjMap`,Object 子类型)在 collect 的 trace 阶段调用。在此之前,持有 `AriaArray`/`AriaHashTable` 局部变量**不会**根化其元素--元素对象需另行根化(或禁用 GC 隔离测试)。
- **`HashTable::upsert` 新条目 value 初始化为 `V{}`**:`Value{}` 是 f64 0.0 非 nil(NanBoxing);需要 nil 的场合调用方 upsert 后显式覆写 `value`。命中已有键时 value 保留(不重置)。
- **`HashTable`/`InternPool` 两块独立分配**:各持 `ctrl_`+`entries_`(HashTable)/`slots_`(InternPool),rehash 时一起重分配 + 重插 + 释放旧。`ctrl_` memset 0xFF(kEmpty)、`slots_` memset 0(nullptr)。**不走 `Array<T>`**(rehash 要按新容量重算位置,memcpy 会放错)。
