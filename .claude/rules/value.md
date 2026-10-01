---
name: aria-value
description: aria 解释器 value 层模块参考：Value（NaN-boxing/TagValue 可切换）、AriaArray、AriaHashTable、value_equal/value_identical 双相等、format_value 系列渲染。读写 src/value/** 或涉及值表示、栈上值布局、类型名打印时使用。
paths:
  - "src/value/**"
---

# value 层模块参考

## `value/` 两种值表示（`USING_NANBOXING` 切换）

切换机制见 `common.hpp`（core.md）：`value/Value.hpp` 经 `#ifdef` 选 `NanBoxing.hpp`（`aria::nanboxing`）或 `TagValue.hpp`（`aria::tagvalue`），并 `using namespace` 进 `aria`。

- `NanBoxing::Value`：把 Nil/Bool/F64/Int/Obj 压进单个 64 位 double（int 为 i48 载荷、指针取低 48 位），`sizeof == 8`；`TagValue::Value`：`Type` tag + union，`sizeof == 16`。
- 二者 API 一致（`from_bool`/`from_f64`/`from_int`/`from_i32`/`from_obj`/`is_*`/`as_*`/`type()`/`type_name()`；已知例外：`is_box()` 仅 NanBoxing 有、`as_i32()` 两表示各一份且全库零调用），均为 trivial + standard-layout（各头 `static_assert` 把关），可 `memcpy`、可入 `FrameStack`。
- **默认初始化不定值**：默认构造 `= default` 为 trivial，默认初始化 `Value v;` 是不定值**并非 nil**，取 nil/真/假统一走工厂 `nil_val()`/`true_val()`/`false_val()`。差异：NanBoxing 走私有 `Value(u64)` 构造；TagValue 走 `Value{}` 零填充 -> `Type::Nil` = 0 即 nil，而 NanBoxing 的 `Value{}` 零填充是 f64 `0.0` 非 nil--零填充值栈时留意（当前无 `Array<Value>::resize` 类零填充消费点，属预防性约束）。
- **NaN 规范化**：两者的 `from_f64` 都把任意 NaN 规范化为硬件 quiet NaN（`0x7ff8...`）--NanBoxing 免其被误判为 boxed 值，且 `===` 按位比较下 `NaN === NaN` 成立依赖两表示共同规范化，任一侧不规范即语义分叉。

## `value/AriaArray.hpp`

`AriaArray : public Array<Value>`：继承 Array 存储/接口，加 `trace(GC&)`（遍历元素 `mark_value`）与值相等原语 `find`/`contains`/`remove`。三者 `==` 语义收口 `value_equal`（嵌套容器按内容递归、数值跨型相等 int 1 == f64 1.0，与哈希键的 `===` / `value_identical` 按类型严格相对）；`remove` 移除**全部**命中（保序一趟压缩），未命中零操作返 false。非 Object，trace 由 owner（ObjList）在 collect 调；语言面 list 的 find/contains/remove 方法体即其薄壳。

## `value/AriaHashTable.hpp`

`AriaHashTable : public HashTable<Value,Value,ValueHash,ValueEq>`：继承 Swiss Table 实现，加 `trace(GC&)`（range-for 走 `const_iterator` 标 key+value，迭代器只看 ctrl 不加载非占用 Entry）。含 `ValueHash`/`ValueEq` 内联包装（仅本处用，转发到 `value_hash`/`value_identical`）；**哈希键用 `===`**（`value_identical` 严格相等：对象按引用、字符串靠 intern 同指针、int 1 与 f64 1.0 不同键）。

## `value/ObjBridge.hpp`

**Value↔Object 耦合辅助的收口头**：收「同时依赖 Value 与 Object 完整类型、必须头内定义（模板）」的辅助函数；非模板的重依赖件（`type_name(Value)`/`value_equal`/`value_hash`/`format_value` 系列）走「`Value.hpp` 声明 + `Value.cpp` 定义」的旧路。独立成头不常驻 `Value.hpp`：模板定义硬需 include `Object.hpp`，而该依赖只服务其用户面（runtime 与 object 两层：builtin 守卫 / 异常载荷判定 / 读路径绑定判别等），不拖累被编译层（常量池）广泛 include 的 `Value.hpp`。

- `try_obj<T>(const Value value)`（`DerivedFromObj` 约束，按值收参）：`is_obj()` + `Object::try_as<T>` 合一的「守卫后使用」一步守卫（非对象/类型不符返 nullptr）。
- `is_callable_value(Value)`：类表成员值是否为「可调用」（闭包或原生）。**读路径绑定判别不走本谓词**（改判 `is_method(Value)` 的 defining class 戳）；本谓词保留为可调用集合的**泛化扩展缝**，现行消费仅 `ObjBoundMethod` ctor ASSERT。
- `is_method(Value)`：类表成员值是否为「方法」--defining class 戳定的方法闭包，读路径（`ObjInstance::load_field`/`LOAD_SUPER_FIELD`）的绑定判别谓词；方法承载形态的**泛化扩展缝**。与 `compile/FnKind.hpp` 的 `is_method(FnKind)` 同名异参、互不相关。

## `value/Value.hpp` / `.cpp`

值操作 `value_hash`/`value_equal`/`value_identical`/`value_less` 声明在此（基于 `type()` + `as_*()`，两表示都编译），定义在 `Value.cpp`。

- **双相等**：`value_equal`（`==` 内容相等，Int/F64 跨类型 IEEE 数值、Obj 调 `Object::equals` 虚函数）、`value_identical`（`===` 严格相等，类型严格、f64 按位、Obj 指针）；哈希键用 `===`。
- **std 容器键设施**：`std::hash<Value>`/`std::equal_to<Value>` 特化在头末，分别转发 `value_hash`/`value_identical`，令 std 容器可以 Value 作键（消费方：`FunctionCtx` 的常量池去重索引 `HashMap<Value,u16>`）。与 `AriaHashTable` 的键语义一致--`===` 而非 `==`，故 int 1 与 f64 1.0、`-0.0` 与 `0.0` 各为不同键。
- **自然序小于** `value_less`（排序底座）：双数值按数值序（NaN 排在一切数值之前保严格弱序）；双字符串按无符号字节序走 `string_view::compare`。域外组合未定义、调用方先域检，list.sort 消费。
- **辅助自由函数**：`is_num(Value)`、`is_truthy(Value)`（Lua 风格真值：仅 nil/false 为假）、`type_name(Value)`（**精确类型名**统一入口，PascalCase：原语走 constexpr 成员、Obj 取对象子类型；区别于成员的 constexpr 粗分类--后者 Obj 一律返 `Obj`；错误消息类型名打印一律用本自由函数）。
- **渲染**：`format_f64(f64)`/`format_value(Value)`（println/str 等显示位与错误渲染用，Obj 走显示位虚 `to_string()`）；`format_value_debug(Value)` 是**非重入**调试渲染（执行跟踪 / 反汇编常量池共用），Obj 走虚 **`debug_repr()`** 而非可重载的 `to_string()`（后者是用户类 `__str__` 的挂载点，可重入 VM 致无限递归）；`debug_repr` 的 override 契约是纯 C++ 惰性渲染（见 `Object.hpp`）。
