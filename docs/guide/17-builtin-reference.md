# 第 17 章 附录：内建参考

本章是速查手册：全局内建函数、各类型方法面、下标与切片规则、`type()` 返回名、常见错误
消息。教程正文（各章）讲了语义与惯用法，这里收口为表格。语法产生式以
[docs/grammar.txt](../grammar.txt) 为准；语义设计见 `.claude/reference/`。

## 全局内建函数（6 个）

| 函数 | 元数 | 行为 |
| :--- | :--- | :--- |
| `type(v)` | 1 | 返回 v 的类型名字符串（见下表） |
| `str(v)` | 1 | 返回 v 的可读渲染字符串（与 `println` 渲染同一套） |
| `println([v])` | 0..1 | 打印 v（省参只输出换行），返回 nil |
| `assert(cond[, msg])` | 1..2 | cond 为真返回 nil；为假抛 `AssertionFailed`。msg 为字符串时进错误消息，非字符串静默忽略 |
| `clock()` | 0 | 返回 f64 单调时钟秒数。**只有两次读数相减有意义**（起点未规定），用于自测耗时 |
| `Error(message[, code])` | 1..2 | 造一个异常对象（`type == "Exception"`）：`message` 为所给字符串原样；`code` 可选，整数错误码原样携带（`e.code()` 即其值）；实参类型不符报 `TypeMismatch`（见[异常章](14-exceptions.md)） |

全部内建都是一等值，可以存进变量、当参数传。用户在模块顶层 `var` 同名绑定会遮蔽内建
（名字解析先查模块全局、再回退内建表），但别这么干。

`type()` 的返回名全集：

| 值 | 返回名 |
| :--- | :--- |
| 整数 / 浮点 / 字符串 / 布尔 / nil | `"Int"` / `"F64"` / `"String"` / `"Bool"` / `"Nil"` |
| 列表 / 映射 / 区间 / 迭代器 | `"List"` / `"Map"` / `"Range"` / `"Iterator"` |
| 函数与闭包 | `"Closure"` |
| 类 / 实例 | `"Class"` / `"Instance"`（不区分类名） |
| 模块 | `"Module"` |
| 协程 | `"Coroutine"` |
| VM 运行期错误装箱 | `"Exception"` |

## coroutine 模块（4 个）

内建模块 `coroutine`（经内建表解析，见第 16 章）。成员函数对协程操作，`co` 一律指
协程值：

| 函数 | 元数 | 行为 |
| :--- | :--- | :--- |
| `coroutine.create(fn)` | 1 | 以 fn 为体建协程（登记不执行），返新协程；fn 非函数报 `TypeMismatch` |
| `coroutine.resume(co[, v...])` | 1..∞ | 首启：其余实参作协程体实参（元数照查）；已挂起：至多 1 个载荷作 `yield` 表达式值。返回 yield 值，协程体完成时返回其返回值 |
| `coroutine.yield([v])` | 0..1 | 挂起当前协程，v 作恢复侧 `resume` 表达式的值；可从任意调用深度发起；主上下文调用报 `YieldOutsideCoroutine` |
| `coroutine.status(co)` | 1 | 返回状态拼写串：`"suspended"` / `"running"` / `"normal"` / `"done"` / `"failed"` |

`co` 非协程报 `TypeMismatch`（`create` 对应报 fn 非函数）。

## string 方法面（17 个 + 6 个算子钩子）

字符串不可变，所有变换产新串。除 `codepoint_at`（码点序号）外，下标与长度一律**字节域**。

| 方法 | 行为 |
| :--- | :--- |
| `upper()` / `lower()` | ASCII 大小写转换 |
| `trim()` | 去首尾 ASCII 空白 |
| `split([sep])` | 带 sep：按分隔符切、保留空段；无参：按连续空白切、丢空段 |
| `find(sub)` | 子串首现字节下标，未命中返 `nil` |
| `contains(sub)` | 子串判定 |
| `replace(old, new)` | 全部替换；old 为空串报 `EmptyPattern` |
| `substring(start[, end])` | 字节区间 `[start, end)`，end 省略到尾；不收负数，越界 / `end < start` 报 `IndexOutOfBounds` |
| `starts_with(p)` / `ends_with(s)` | 前缀 / 尾缀判定 |
| `size()` | 字节数（码点数用 `chars().size()`） |
| `is_empty()` | 空串谓词 |
| `chars()` | 逐码点切出的单字符列表（非法字节产替换码点） |
| `codepoint_at(i)` | 第 i 个码点的码点值 |
| `to_int()` | 整串十进制整数解析，失败返 `nil` |
| `to_float()` | 整串浮点解析（收 `1e3` 形态），失败返 `nil` |
| `iter()` | 码点域迭代器 |

算子钩子（内建形态，见第 13 章）：`__add__`（两侧皆 String 的 `+`）、`__mul__`（String *
非负整数 Int 的重复）、`__lt__` / `__le__` / `__gt__` / `__ge__`（无符号字节序比较）。

## list 方法面（14 个 + 2 个算子钩子）

变更方法一律**就地改**；返回值列明。

| 方法 | 行为 |
| :--- | :--- |
| `push(x)` | 追加末尾，返 nil |
| `pop()` | 移除并返回末元素；空表报 `IndexOutOfBounds` |
| `insert(i, x)` | 位置 i 前插入；负数从尾计数，`i == size` 合法（即追加） |
| `remove(x)` | 移除**全部** `==` 命中元素，返是否命中（Bool） |
| `remove_at(i)` | 按位置移除并返回元素；负数从尾计数 |
| `clear()` | 原地清空，返 nil |
| `sort()` | 就地稳定升序；限全数值或全字符串，混域报 `TypeMismatch`；NaN 排最前 |
| `reverse()` | 就地整段反转，返 nil |
| `find(x)` | 首个 `==` 命中下标，未命中返 `nil` |
| `contains(x)` | `==` 成员判定 |
| `size()` / `is_empty()` | 元素数 / 空表谓词 |
| `join(sep)` | 元素渲染后以 sep 连接产新串 |
| `iter()` | 元素迭代器 |

算子钩子（内建形态，见第 13 章）：`__add__`（两侧皆 List 的 `+`，产浅拷新表）、`__mul__`
（List * 非负整数 Int 的重复，产新表；乘数校验同 string）。

## map 方法面（10 个）

键判等一律 `===`（第 7 章）。

| 方法 | 行为 |
| :--- | :--- |
| `size()` / `is_empty()` | 键值对数 / 空表谓词 |
| `has(key)` | 键存在判定 |
| `get(key)` | 读值，未命中返 `nil`（与「值为 nil」不可分，分辨用 `has`） |
| `keys()` / `values()` / `pairs()` | 各铸新列表快照；`pairs()` 元素是 `[k, v]`；**顺序未规定** |
| `remove(key)` | 移除命中键，返是否命中（Bool） |
| `clear()` | 原地清空，返 nil |
| `iter()` | 迭代器，每步产出 `[k, v]` |

## range 与 iterator

| 类型 | 方法 | 行为 |
| :--- | :--- | :--- |
| range | `iter()` | 整数迭代器（方向由端点定）。range 方法面目前仅此一个 |
| iterator | `has_next()` | 是否还有值 |
| iterator | `next()` | 取下一值并推进；耗尽后再调报 `IterationExhausted` |

## 下标与切片速查

| 类型 | 整数下标 | range 下标（切片） |
| :--- | :--- | :--- |
| list | 读写皆可；负数从尾计数；越界报错；写不自动增长 | 产新表只读；闭 / 半开 / 无上界 / 负端点 / 倒序段；端点越界报错（起点恰等长度合法） |
| string | 读：切出单字节串；负数从尾计数 | 与 list 同口径（字节域、倒序段） |
| map | 任意值键、`===` 判等；读 miss 报 `KeyError`；写 miss 即新增 | 无 |

无步长切片；`substring` 是方法不是下标，且不收负数。

## 常见错误消息速查

编译期错误带 `文件:行:列:` 位置，前缀 `Syntax:` / `Semantic:`；程序不执行。常见样例：

```text
hello.aria:1:9: Syntax: UnterminatedString unterminated string: line break in literal
hello.aria:1:10: Syntax: InvalidEscape invalid escape sequence '\q'
hello.aria:1:12: Syntax: ExpectedExpression expected expression, got '.'
hello.aria:1:15: Syntax: DefaultAfterPlain non-default parameter after default parameter
hello.aria:1:14: Syntax: VarargsNotLast varargs '...' must be the last parameter
hello.aria:1:9: Semantic: NumberOutOfRange integer literal 140737488355328 out of range
hello.aria:1:11: Semantic: BreakOutsideLoop 'break' outside loop
hello.aria:1:27: Semantic: SuperOutsideMethod 'super' outside method
hello.aria:1:1: Semantic: TryWithoutHandler 'try' requires a catch clause
```

运行期错误前缀 `Runtime:`（用户与 VM 错误）/ `Internal:`（assert 失败）/ `Resource:`
（模块加载失败），未捕获时附逐帧 `at 名字 (文件:行)` 堆栈跟踪。常见样例（省略堆栈行）：

```text
Runtime: DivisionByZero integer division by zero
Runtime: ModuloByZero integer modulo by zero
Runtime: IndexOutOfBounds list index 5 out of range
Runtime: IndexOutOfBounds slice range 1..9 out of range
Runtime: IndexOutOfBounds pop from empty list
Runtime: IndexOutOfBounds substring range 3..9 out of range
Runtime: KeyError map key not found: "nope"
Runtime: TypeMismatch __add__ requires two strings, got String and Int
Runtime: TypeMismatch operator '<' requires numbers, got Int and String
Runtime: TypeMismatch range bounds must be integers, got F64 and Int
Runtime: TypeMismatch list index must be an integer, got Range
Runtime: TypeMismatch type Module does not support field assignment
Runtime: TypeMismatch sort requires all numbers or all strings, got Int and String
Runtime: TypeMismatch argument must be a coroutine, got Int
Runtime: UndefinedVariable undefined global 'nope'
Runtime: UndefinedProperty <class Box> has no member '__add__'
Runtime: UndefinedProperty <module util> has no member 'absent'
Runtime: WrongArity function expects 2 arguments, got 1
Runtime: MatchNoArm no arm matched
Runtime: IterationExhausted iterator exhausted
Runtime: StackOverflow call frame stack overflow
Runtime: CallNonCallable type Int does not support '__call__'
Runtime: EmptyPattern split separator must not be empty
Runtime: EmptyPattern replace pattern must not be empty
Runtime: ResumeDeadCoroutine cannot resume a dead coroutine
Runtime: ResumeNonSuspendedCoroutine cannot resume a non-suspended coroutine
Runtime: YieldOutsideCoroutine cannot yield outside a coroutine
Internal: AssertionFailed assertion failed
Resource: ModuleNotFound module not found: 'nope'
```

## 去处

- 语法产生式：[docs/grammar.txt](../grammar.txt)（语法面单一事实源；语义设计见 `.claude/reference/`）。
- 实现与构建：仓库根 `README.md`。
- 本章缺的东西（`enumerate` 一类迭代辅助、更多 range 方法）语言层尚未提供，教程不预先
  书写。

---

[上一章：协程](16-coroutines.md) · [返回目录](README.md)
