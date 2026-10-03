---
name: aria-object
description: aria 解释器 object 层模块参考：Object 基类与 is<T>/as<T>/try_as<T> 约定、ObjString（SSO+intern）、ObjFunction（含捕获描述表 UpvalueDesc）、ObjClosure/ObjUpvalue（M4 闭包）、ObjClass/ObjInstance/ObjBoundMethod（M5 类）、ObjList/ObjMap/ObjRange（集合值）、object/iterator 迭代器族、ObjNativeFn、ObjException、ObjModule、ObjMovement（执行上下文对象，文件住 runtime 层）。读写 src/object/** 或涉及对象子类型、GC 根纪律、intern 驻留时使用。
paths:
  - "src/object/**"
---

# object 层模块参考

本层只做**对象的表示与协议**。**每个类型的权威契约在其头注释**（字段 / 访问器 / 工厂守卫 / 各 override 的取舍都在那里）；本文件 = 类型地图 + 协议缝 + 跨类型规则，不复述头注释。

## 类型地图

- `Object`（`object/Object.hpp`）：所有 GC 对象的基类。持 `ObjType` 枚举、地址哈希（可变对象）/ 内容哈希（不可变对象）两类 ctor、`is<T>`/`as<T>`/`try_as<T>`、协议虚函数族（见下）。两 ctor 即「缓存哈希」的**构造期分发**--不可变对象传算好的内容哈希、可变对象取地址哈希；故 `hash_` 留在基类，查询期直接 `o->hash()`，无需按类型分发的自由函数。
- `Object::type_name()` 非虚，纯由 `type_` 决定；类型名映射单一来源 = `to_string(ObjType)`，全项目类型名 PascalCase（原语 `Nil`/`Bool`/`Int`/`F64`/`Obj`，对象 `String`/`NativeFn`/...）。
- `Object.hpp` include `value/Value.hpp`：基类的**成员访问 / 运算符协议虚函数**签名需要 `Value` 完整类型（Value.hpp -> boxing 头 -> common.hpp，不依赖 Object，无 include 环；子类型头早已经 AriaHashTable 等 value 头拉入 Value，非新增暴露）。
- `ObjString`（`ObjString.hpp`）：SSO 字符串 + FNV-1a 哈希 + intern 驻留；显示位与调试位唯一分叉的子类型。铸造入口 = `new_string` 工厂族（先查驻留命中返回已有串、未命中才分配+insert），含两段重载 `new_string(gc, lhs, rhs, hash)`（先查后拼：命中零拷贝，哈希为 `hash_str(hash_str(lhs), rhs)` 续算终态）。
- `ObjFunction`（`ObjFunction.hpp`）：`CodeUnit` 值成员 + 所属模块 + 名 + 元数（固定 / 必传 / varargs 三量）+ 捕获描述表 `UpvalueDesc`。
- `ObjUpvalue`（`ObjUpvalue.hpp`）：闭包「捕获即引用」的载体（open 指值栈槽 / closed 自持值）。
- `ObjClosure`（`ObjClosure.hpp`）：被包函数 + upvalue 数组 + `defining_class_`；运行期一律以闭包进帧，`ObjFunction` 退为常量池内部物。
- `ObjClass`（`ObjClass.hpp`）：def 类的运行期载体（类级静态表 + superclass 单链，实例字段不在类上）。
- `ObjInstance`（`ObjInstance.hpp`）：类实例化的产物（所属类 + 实例字段表）。
- `ObjBoundMethod`（`ObjBoundMethod.hpp`）：被绑定方法值 + 接收者。
- `ObjNativeFn`（`ObjNativeFn.hpp`）：把 C++ `NativeFn` 包成 aria `Value`（builtins / 嵌入 API 用）。
- `ObjModule`（`ObjModule.hpp`）：模块 = 命名空间（一个源文件一个模块），非类。
- `ObjException`（`ObjException.hpp`）：VM 检测错误 / 原生报错的装箱载荷；注意 aria 的 `throw` 抛任意 `Value`，不限定本类型。成员解析委托 Exception bootstrap 类（`vm.exception_class()`，沿链达 Object 根；`message`/`code` face 经此触达、按接收者分派：本类型读原生成员，链上实例经 `load_field` 读 `_message`/`_code` 字段；与其他内建对象经 VM 访问器取自身 bootstrap 类同款，本类不持类指针）。`code_` 是错误码数字（i64 单一存储：VM 报错 = 注册表序号，`Error(msg, code)` 码参 = 用户所给 int），双视图消费：`code()` 转 ErrorCode（C++ 消费方，VM 路径 cast 保真）、`numeric_code()` 保 i64（语言面）。
- `ObjList` / `ObjMap` / `ObjRange`：`[...]` / `{...}` / `a..b` 字面量的运行期载体。
- 迭代器族（`object/iterator/`）：`ObjIterator` 基类 + 每源一个小子类 `ObjListIterator` / `ObjStringIterator` / `ObjMapIterator` / `ObjRangeIterator`。
- `ObjMovement`（`runtime/ObjMovement.hpp`，文件住 runtime 层）：执行上下文对象（`ObjType::MOVEMENT`，主上下文与协程统一本型，主上下文为 ctor 首笔分配的唯一实例）。`type()` 报 `Coroutine`、`debug_repr()` 报 `<coroutine {状态小写拼写}>` 动态形（`ExecState` 五态：suspended/normal/running/done/failed，`status` 直接投影 `state_` 不做谓词派生；主上下文亦参与换位但无人读）。成员/下标/算子协议全落基类默认（身份判等、不支持成员访问）--coroutine 值的可取行为。trace 自标值栈/帧/开链/挂起载荷并经 `mark_object(previous_)` 沿 resume 链级联；机制细节见 runtime.md。
- `EqualGuard.hpp` / `PrintGuard.hpp`：递归 `equals` / `debug_repr` 的 thread_local 环守卫（容器入口挂；命中即视为相等 / 截断 `[...]`）。
- `value/ObjBridge.hpp`：Value↔Object 耦合辅助的收口头（`try_as_obj<T>`、`is_callable_value`、`is_method`）。

## 协议虚函数族

声明在 `Object.hpp`，基类默认体在 `Object.cpp`（出声明因 `vm.fail` 是 AriaVM.hpp 内模板、两头互不 include）。

**类型头声明序**（各子类型统一，`.cpp` 定义序同序）：特殊成员（ctor / dtor / deleted 拷贝移动）-> 非虚访问器与写入器 -> `Object` 协议 override（**严格按 `Object.hpp` 的声明序**：`trace`、`size`、`equals`、`debug_repr`、`to_string`、`load_field`、`load_field_bound`、`store_field`、`load_index`、`store_index`、11 个 `op_*_impl`）-> 本类型自有虚函数（如 `ObjIterator` 的 `has_next` / `next`）-> private 辅助函数 -> **数据成员（类定义收尾）**。故每个类恒为「一个 public 段 + 一个收尾的 private 段」（`ObjIterator` 收尾段是 protected，它是抽象基类）。

**仅类型内部消费的辅助留 private**（`ObjString::slice`、`ObjList::slice`、`ObjClass::find_field`），不按「可能有用」外放；const 重载的容器访问器按实际消费面取舍（无 const 消费者的不加）。`Object` 的四个字段（GC 侵入式链的 `next_` + `hash_` / `type_` / `is_marked_`）同样收在收尾 private 段，GC 取字段级访问经 `friend class GC`（sweep / free_all 直接取 `&next_` 摘链，链头 `objects_head_` 住 GC；其余三字段对外只经访问器）。

### 错误与文案契约

- **错误通道 = native fn 契约模型**：签名一律收 `AriaVM&` 单一句柄（分配经 `vm.gc()`、报错一行 `vm.fail(code, fmt, ...)`）。
- 协议失败时**自己 fail**--载荷直接入 `*current_` 挂起错误寄存器（构造即入寄存器即被 VM 根 tracer 标根，无「返回值在途」的白色无根窗口），**返回值只留信号**：
  - load 族 `Opt<Value>`：nullopt ⟺ 已 fail；somed 才是命中，命中 nil 亦 somed。
  - store 族 `bool`：false ⟺ 已 fail；失败出口经 `FailSignal` 哨兵一行化--override 一律 `return vm.fail(...)`，哨兵按站点返回类型转 nullopt / false / nullptr。
- 该模型与 native fn 的 bool 契约、`call_value` 族同构，对齐 CPython PyErr 模型。
- **消息文案由最知道语境的一方就地烘焙**（同 CPython listobject.c）：各 override 用自身细节拼（渲染值走非重入 `format_value_debug`，不用可重载 `to_string`，防未来语言级 `__str__` 重入）。
- 组合场景（实例委托类链、super 站点）直接**委托协议** `ObjClass::load_field`--命中值原样回传，miss 的类措辞 fail 随协议传播（成员表在类链上，文案随宿主，组合方不重复烘焙）。
- 契约纪律：至多 fail 一次、fail 后立即返回；调用方（VM）保证接收者「栈即根」（peek 不弹）跨协议内 GC 点。
- 基类默认体一律「本类型不支持」，fail 后返失败信号。

### 四条缝

- **① `load_field` / `store_field`**：命名成员的裸查找与写入协议（`load_field` = 裸查找原语：返回命中原值、永不铸 `ObjBoundMethod`、命中零分配；LOAD/STORE_FIELD 族指令统一分派点，VM 不按子类型 switch 分型）。`load_field` 消费方两类：`run_prepare_method`（`PREPARE_METHOD` 的成员解析缝，解析先于实参求值）与实例 11 个 `op_*_impl`（按钩子名取 `__add__`/`__call__` 等）。override：`ObjInstance` / `ObjClass` / `ObjModule` / `ObjException` 与内置集合 / 迭代器（见下）。
- **①′ `load_field_bound`**：命名成员读取的**绑定形态**（同一趟裸查找命中后按宿主决定绑定：实例 fields 命中原样直读、类链命中经 `is_method` 现场绑；内置容器/迭代器/异常无条件绑）。消费方 = `run_load_field`/`run_load_this_field`（LOAD_FIELD/LOAD_THIS_FIELD 的统一执行体）。**基类默认 = 直接委托 `load_field`**（类/模块等读取本就不绑定，照取命中原值，未 override 类型读行为即裸查找）。
- **①' `load_index` / `store_index`**：下标读写协议（VM 侧执行体 `run_load_index` / `run_store_index`）。容器 override 直接 `vm.fail` 自选错误码（IndexOutOfBounds / KeyError），错误细节（越界值、键）就地拼进文案。
- **② 算子与调用协议 `op_*_impl(AriaVM&) -> Opt<Value>`**（算术五 + 比较四 + 一元负 + `op_call_impl`，共 11 个）：**取实现，不执行**--回答「本对象上该算子对应的可调用值」（不是算好的结果），VM 的 `run_binary_operator<Op>` / `run_negate` / `call_value` 取到后按调用形态调它（调用区槽 0 保持 receiver）。非 const（取实现可能物化绑定）。
- ②的基类默认直接 fail（`type X does not support '<钩子名>'`，码 TypeMismatch；调用同形但码 CallNonCallable）。实现者：①**实例**（11 个 override，各自按钩子名（VM 常量串表，见 `runtime.md`）裸查找 `load_field`--实例 fields 可遮蔽，再类链取）；②**内置 string**（6 个算子直读实现格 `String*Fn`）与**内置 list**（加/乘 2 个 `List*Fn`）--bootstrap 期从类表按名拷入并 ASSERT 一致，免每次过类表查找；③其余类型不实现即报错。
- 钩子名是**语言级事实**（拼写注册表 `runtime/str_table.hpp`，调用钩子 `__call__`）；方法仍在类表里（`"a".__add__("b")` 读路径不变）。
- `op_call_impl` 的消费点 = `AriaVM::call_value` 的 switch `default` 臂：取到后用**同一调用区**递归分发（`[callee, a1..aN]` 恰是 `[this, args]`）；非对象 callee 同码同款文案（`type X does not support '__call__'`，码 CallNonCallable）。
- **钩子自指/成环不兜底**（拍板）：`d.__call__ = d` 或 `a.__call__ = b; b.__call__ = a` 会无穷重入 `call_value` 直到 C++ 栈溢出（SIGSEGV，无错误消息）。按「手写死循环同类」处理、后果由使用者承担--不加自指检测、不加重入深度上限、不改查找路径。

### 内置类型的成员面

- **内置容器 / 迭代器（string/list/map/range/iterator）与异常（exception）的 `load_field` / `load_field_bound` 共享同一趟裸查找**（权威说明，各子类头不复述）：两缝同委托自身 bootstrap 类表（`vm.string_class()` / `vm.list_class()` / `vm.exception_class()` ...）沿链查表，miss 的类措辞 fail 随协议透传；`load_field` 裸读命中直取类表原生值；`load_field_bound` 命中即自持 `new_bound_method` 恒绑 this（内置类表条目全为原生、恒为方法，判别无须戳）。
- 内置类型的 `store_field` 不 override（基类默认即正确行为--不可变成员面）。
- 方法面构造口 = 各宿主类公有静态 `XxxClass::make_class`（建类挂 super 并经 `Builtin::register_class_methods` 底座装载方法面，`ObjectClass` 根类免 super），方法清单即各 `XxxClass.cpp` 匿名命名空间的方法表；全局面编排口 = `Builtin::register_builtins`（住 `runtime/builtins/Builtin.{hpp,cpp}`）。

## 跨类型规则

### 工厂守卫

- **工厂守卫原则**（全部 `new_<type>` 工厂通用，各工厂头注释不复述）：**每方只守自己创建的**。
- 工厂不替调用方守卫**入参**（入参非本工厂创建；`name` 经 intern 是 weak root，`klass`/`module`/`super` 可能尚未入任何根）；工厂内部新建的对象（便捷重载内 intern 的串）自带 `make_guard` 自守。
- 故调用方须在调用前自行根化自己传入的对象入参；工厂返回对象白色无根、建成即须发布进根（写回值栈槽 / 链入 VM 开链）。

### `defining_class_` 戳规则

- 闭包上的 `defining_class_` **一职双任**：super 来源（`LOAD_SUPER_FIELD` 从 `frame.closure->defining_class()` 直读）+ **方法性标记**（`is_method()`；读路径 `ObjInstance::load_field_bound` / `LOAD_SUPER_FIELD` 据非空判绑 this）。
- **判别按戳不按值类型**：MAKE_METHOD 注册时戳、MAKE_STATIC / 类上赋值不戳 ⟹ 静态槽原值直读；赋值闭包无戳 ⟹ 改写后按静态读原值、不再绑定（方法性随值携带）。
- **挂闭包而非共享的 `ObjFunction` 常量**：函数体内 def 执行 N 次产生 N 个类共用同一 fn 常量，superclass 运行期可重绑，戳共享 fn 会跨实例串错 super 链。
- 非方法闭包（静态方法 fun / lambda / 其余）恒 nullptr。

### `init_` 派生

- `ObjClass::init_` 是 `Value`（闭包 / 原生皆可），实例化统一走 `call_value` 分发；类上赋非可调用值亦放行，实例化时 `call_value` 报 CallNonCallable 兜底。
- 写点**全在对象内**：构造期自 super 派生（**快照语义**：此后父 init 变更不传导；Object 根 super == nullptr 出厂 nil，由 bootstrap 经 `set_field` 设）+ `set_field` 命中 "init" 同步。
- **工厂纯分配不 seed**；无 `set_init` 可变器；`call_value` CLASS 分支消费。
- `set_field` 是类成员**创建路径的唯一公开写入口**（MAKE_STATIC / MAKE_METHOD 注册、bootstrap 设 init、`store_field` 落表三路共用；落本类自身表不沿链，命中 "init" 同步 `init_`）；`find_field` 是纯查询内部底座（沿 super 链、无分配、不 fail、私有）。

### 类成员读写与绑定方法不缓存

- **类成员读写取 Python/JS 式读穿透、写遮蔽**：读沿 super 链 fall-through；写落**接收类自身**表（继承名新建遮蔽键、本类已有原槽更新、沿链全 miss 的新名字亦落接收类，动态新增允许），父表不动。
- **绑定方法不缓存**：`ObjInstance::load_field_bound` 命中方法戳闭包即**每次访问现场绑定**新 `ObjBoundMethod`，**不写回 fields**。
- 缓存会让「类 / 父类改写方法」对已取过方法的实例陈旧、monkey patch 半可用且难解释；不缓存换来改写对既有实例立即生效（monkey patch 完整）与 `fields_` 回归纯字段。
- 方法值是一等值省不掉；**调用路径**（`PREPARE_METHOD` + 实例 `op_*_impl`）经裸查找 `load_field` 走零分配、不绑定形态。

### 迭代器族契约

- `ObjIterator` 是**唯一非 final 的 Object 子类型**（引擎缝基类，每源小子类继承；其余子类型全 final。`ObjType::ITERATOR`，语言层单数类型名 `type(it)` 恒 `"Iterator"`）。
- 基类钉纯虚契约：`has_next() const noexcept -> bool`（纯查询、无分配无 fail，故不收 vm）、`next(AriaVM&) -> Opt<Value>`（取下一元素并推进；越界一行 `return vm.fail(IterationExhausted)`）、`trace`（各子类标各自的源，纯虚钉住忘标 = 编译错）、`size()`。
- `load_field` / `load_field_bound` override 基类一次、全子类共享（走 Iterator bootstrap 类表）；`debug_repr` 渲染 `<iterator>`；`equals` 默认地址判等。
- 每源一个小子类、各持自然游标；**range 迭代器是唯一无源对象者**（range 不可变，ctor 期把端点拷成标量自足，`trace` 空体；方向构造期定向，from > to 即倒序）。
- map 迭代序 unspecified，map 迭代器产出 `[k, v]` 二元 list；迭代中变更容器不设防（v1 不承诺）。序不承诺的理由：非定序哈希表是性能上的正确选择，且这一边缘特性各语言/实现标准不一，用户不应依赖。
- **协议三方法（`iter`/`has_next`/`next`）是类表方法，不为迭代协议另开 Object 虚函数**：① 方法必须一等（`var n = it.next; n()` 虚函数做不了）；② 与用户类统一，单一派发路径；③ VM 内部无迭代消费者（解构走下标、GC 不迭代），双通道纯漂移风险；④ 能实现 `iter` 的对象必已支持成员查找、必已有方法表。
- **形态判据（每源子类 + 纯虚契约，而非单一结构体 + switch）**：单一 `ObjIterator{source, cursor}` 兼多义、按类型 `switch` 分派，违背引擎缝「不按子类型分型」的架构；跨语言（Python/JS/Java/C#/C++）均为每源独立迭代器 + 统一虚契约。C++ STL 迭代器不能直接当语言值（GC 一等 Value、指针悬于元素缓冲扩容、用户类需统一协议），但其「每类型自己的表示 + 统一契约」思想即本方案的运行期对应物。迭代器对象不可省：游标状态须随迭代器走，容器不可自带游标（否则嵌套遍历同一容器互相串扰）。
- 语言方法面（has_next/next）住 `runtime/builtins/IteratorClass`。

### 模块身份

- `ObjModule` 的绝对路径（= VM 模块表查重键）= `dir_ + "/" + name_ + ".aria"`，切分收口 `fs::module_name_and_dir`--`name_` = basename 去 `.aria` 后缀（stem）、`dir_` = dirname。
- 即路径的 dirname/stem 切分，`name_` 恒为单段 stem，不支持「相对源根的多段路径」语义、不依赖源根概念；两指针**恒非空、内容可空**（合成顶层 `<script>` / REPL 等以非空 intern 串构造）。
- `abs_path()` 即时合成：`dir_` 内容空 -> 返空串（无目录锚点）；`name_` 内容空 -> 仅返 `dir_`；否则拼接（供 IMPORT 相对导入取 dirname 作基）。
- `format_location(line)` = 未捕获堆栈跟踪 at 行用的位置串（`"<loc>:<line>"`）：文件模块取 `abs_path()`；合成模块（`name_` 以 `<` 开头--`aria.hpp` 约定尖括号名标记 VM 合成实体，其 `abs_path()` 会拼出伪路径）与无目录锚点（`abs_path()` 空）退化为 `name_`。纯 `std::format` 拼接、不分配 GC 对象（`unwind` 物化路径依赖「无 GC 分配点」）。
- **无加载状态字段**：加载事实源 = VM 模块表成员资格（未入表 = 未加载；入表 = 已编译待 run-once 或已跑完）。
- 成员协议：顶层绑定即成员（`load_field` 查 `globals_` 直读，不另设 export 声明；顶层函数值恒非方法、不绑 this；nil 值绑定与 miss 由 find 空态区分）；`store_field` 恒拒（模块成员只读--越模块写会隐式创建他人未声明全局）。

### 切片段两类型共享

- `resolve_slice_bounds(range, size)`（`ObjRange.cpp` 收口）是 `ObjList` 与 `ObjString` 切片的共同解析口，返 `Opt<SliceSegment>{start, count, is_reversed}`。
- 长度与方向的折算全在解析口：有上界形态两端点各从尾计数、方向由归一化端点大小关系自带（与 range 迭代同一判据）、不含上界少走迭代序末元素、两端相等即空段；**无上界形态 `i..` 走后缀语义**，起点从尾计数后允许 `== size` 得空段（解构 rest 的空尾据此成立）。
- 端点越界与空容器同为 nullopt，唯一失败文案 `slice range {} out of range`（插被请求的 range 渲染，list 与 string 同串）。
- list 切片铸新 list 段拷（正序整段一次拷 / 倒序经 `Array::copy_reversed_from`）；string 切片域仍是字节，倒序段产出字节倒排串（多字节输入下非合法 UTF-8）。

### GC / 根纪律

- 协议内可分配（fail 装箱 `new_exception`、`new_bound_method`），调用方保证接收者「栈即根」（peek 不弹）。
- 工厂返回的白色无根对象须发布进根--写回原槽（`run_load_field` / `run_load_index`）或链入 VM 开链（upvalue，链上节点经宿主 `ObjMovement` 的 trace 保命）。
- `Object::equals` 契约须 **GC-pure**（EQUAL 在 off-stack 裸局部上比较，触发 collect 会回收操作数）。
- 递归比较 / 渲染子值的容器入口须挂 `EqualGuard` / `PrintGuard` 防环（重遇同对视为相等 / 截断 `[...]`）。
- `module_` 回指与 `module->entry_` 成环，mark-sweep 三色标记破环。

### 类型判断 / 转换约定（强制）

- 纯谓词（断言 / fail-fast / 不需要指针）用 `obj->is<T>()`。
- 「守卫后使用」成对场景优先一步 `obj->try_as<T>()`（类型只写一次，DEBUG 单次 dynamic_cast 优于成对的两次）。
- `switch (obj->type())` 臂内等静态已知场合用 `obj->as<T>()`（NDEBUG 裸 cast 不校验）；多分支枚举分派（如 `AriaVM::call_value`）天然合理，保留枚举写法。
- **禁写 `obj->type() == ObjType::X` + `static_cast<X*>(obj)`**--三件套自文档化且 DEBUG 享 dynamic_cast 校验。三件套是 Object 的成员模板，子类型禁定义同名成员（成员调用从派生类作用域起查找，同名即遮蔽）。
- 从 `Value` 出发的「守卫后使用」用 `value/ObjBridge.hpp` 的 `try_as_obj<T>(v)` 一步收口（非对象 / 类型不符返 nullptr）；分步场合仍先 `v.is_obj()` 再 `v.as_obj()->is/as<X>()`。
