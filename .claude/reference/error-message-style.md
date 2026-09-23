# 错误消息文案规范

> 适用面：`src/**` 里一切**面向用户的输出文案** -- 编译期报错（Lexer / Parser / CodeGen）、运行期报错（AriaVM / builtins / object 协议）、CLI 的 help 与 usage、`fatal_error` 前缀，以及源码内的 ASSERT / static_assert 文案。注释与文档不受约束（注释一律中文）。
> 机械门禁：`tools/check_error_messages.py`（`tools/hooks/pre-commit` 壳，启用方式同 `commit-msg`）。通则与家族表的语义判断仍靠人读。

## 1. 消息形态（沿用既有设计，本节只作定位）

| 阶段 | 形态 | 位置载体 |
| :--- | :--- | :--- |
| 编译期 | `path:line:col: Category: Code detail` | 位置串前缀（编译错误无堆栈，位置是唯一锚点） |
| 运行期 | `Category: Code detail` | 未捕获出口的逐帧 `at <fn> (<mod>:line)` 行 |

`Category` / `Code` 是 `ErrorCategory` / `ErrorCode` 的枚举名，经 X-Macro 的 `#` 派生（`src/error/ErrorCode.hpp`）--**单一事实源，不另加中文名表**。故 `detail` 一律英文：换语言要连壳一起换，而壳改中文需再手写一张 55 码 + 6 类别的名表，与「名字串经 `#` 派生、无第二处手写」的设计直接冲突。

`detail` **不带位置**：位置由壳或 `at` 行承载。同理 `detail` 不重复函数名（函数可匿名），归属由 `at` 行给出。

## 2. `detail` 通则

1. **英文 ASCII-only**：禁 CJK、禁全角标点。
2. **小写起首、无句尾标点**：detail 紧跟在 `Code ` 之后，故首字符小写（以 `{}` 占位或类型名开头的天然符合）。句尾不写 `.`/`。`。
3. **只写「上下文 + 期望 + 实际」**：不写解释性从句、不堆多条规则、不写教程。规则本体属文法与注释层。
4. **名字单引号、值走 `format_value_debug`**：标识符 / 模块路径 / 钩子名写 `'name'`；被渲染的值用 `format_value_debug`（字符串自带双引号，与 `'name'` 不混）。
5. **不暴露内部术语**：token 类型名（吐源码拼写）、字节码概念（「回边」）、编码宽度（「64KB」）、C++ 类型名一律不出现。
6. **上限类说人话**：`too many arguments (max 255)`，不写「跳转偏移超过 64KB」。
7. **码名是机器标签，detail 须能独立读懂**：允许与码名语义重叠（`UncaughtException uncaught exception: X` 的重叠保留--删了 detail 就只有机器标签）。
8. **每语义只许一种形状**：见 §3。不因「就近完整」另起措辞；措辞漂移即缺陷源。

## 3. 句式家族表

| 语义 | 唯一形状 | 例 |
| :--- | :--- | :--- |
| 语法期望 | `expected <what>, got <spelling\|end of file>` | `expected ')', got '}'` |
| 元数 | `[<name> ]expects <spec>, got <n>` | `push expects 1 argument, got 0` |
| 实参/下标类型 | `<what> must be <T>, got <actual>` | `list index must be an integer, got F64` |
| 值域 | `<what> must be <T>, got <actual>` | `range bounds must be integers, got F64 and Int` |
| 越界 | `<what> <value\|range> out of range` | `list index 5 out of range`、`slice range 3..9 out of range` |
| 空容器 | `<what> from empty <container>` | `pop from empty list` |
| 空模式串 | `<what> must not be empty` | `split separator must not be empty` |
| 缺键 | `map key not found: <value>` | `map key not found: "nope"` |
| 协议不支持 | `type <T> does not support <op>` | `type List does not support '__add__'` |
| 成员缺失 | `<recv repr> has no member '<name>'` | `<class Box> has no member 'x'` |
| 未定义名 | `undefined variable '<n>'` / `undefined global '<n>'` | |
| 编译语义 | `'<kw>' outside <context>` | `'break' outside loop`、`'this' outside method` |
| 模块 | `module not found: '<path>'` / `failed to load module '<path>': <why>` / `cannot read source file '<path>'` | |
| 除零 / 迭代器 | `integer division by zero` / `integer modulo by zero` / `iterator exhausted` | |

**`<spec>` 取值集（元数族）**：`no arguments` / `1 argument` / `N arguments` / `N or M arguments` / `at least N arguments`。单数为 `1 argument`，其余一律 `arguments`（复数形）。措辞由 `AriaVM::arity_error` 一处构造，调用点只给数量与名，不得手写格式串。

**`<op>` 取值（协议族）**：`field access` / `field assignment` / `subscript access` / `subscript assignment` / `'<钩子名>'`（如 `'__add__'`、`'__call__'`）。读形态与写形态各占一个 op，不合并（`field access` 与 `field assignment` 是可分辨的两种失败）。

**类型名拼写分两域**（都是约定，不是疏漏）：协议族用语言公开拼写（PascalCase，同 `type(x)` 的返回）--`type List does not support '__add__'`；下标/参数族用**小写限定词**（语法上的定语名词，读作「某个 list 下标」）--`list index must be an integer, got F64`、`range bounds must be integers, got Int and F64`。方法名保持自身拼写（`insert index ...`、`remove_at index ...`、`substring index ...`、`codepoint index ...`）。

**三类「类型不符」的边界**（同为 TypeMismatch，但场景不同，各走各形状）：

| 场景 | 形状 | 例 |
| :--- | :--- | :--- |
| 无该协议（基类默认 / 内置类型未实现） | `type <T> does not support <op>` | `type List does not support '__add__'` |
| 有钩子但实参类型错 | `<hook> requires <T>, got <actual>` | `__lt__ requires two strings, got String and Int` |
| 数值内建路径（无钩子可指） | `operator '<sym>' requires numbers, got <a> and <b>` | `operator '+' requires numbers, got Nil and Int` |

## 4. ASSERT / static_assert 文案

`ASSERT(cond, msg)` 展开为 `[file:line] Assert failed in <func>(): <msg>`（见 `src/common.hpp`），**位置与函数名已由宏给出**。故断言文案：

- **不写封闭实体身份**：不写 `Class::method: `、`Class: `、`Class method` 这类前缀，也不写「在哪个函数里」的措辞。消息只写「被检查的对象/量 + 违反的不变式」。
  - 反例：`ASSERT(lvalue_mode_ == Load, "CodeGen::emit_expr: lvalue_mode_ must be Load")` -- `CodeGen::emit_expr` 由宏的 `%s()` 给出，是重复。
  - 正例：`ASSERT(lvalue_mode_ == Load, "lvalue_mode_ must be Load (emit_lvalue did not restore it after dispatch)")`。
- **主语可用类型/量名**：`ErrorCode out of range`、`src must not be null`、`slot-1 is not a class (malformed stack)` -- 这些是**被检查的对象**，不是封闭函数身份（该处封闭函数是 `to_string` / 构造函数）。
- **`static_assert` 例外**：编译期断言只给 file:line、不给函数名，故类型/组件名可作主语保留（如 `InternPool: Alloc must satisfy TrivialAllocator`）。
- 机械门禁覆盖「身份前缀」一项（规则 3），语义（消息是否真的说清了什么坏了）仍靠人读。

## 5. 与其它裁定的边界

- **词法各变体保留字面**（`compile/lexer-notes.md` 的「相邻报错变体字面重复优于模板抽段」继续有效）：那条管的是**各变体原因各不相同**（`invalid escape` vs `unterminated string`，每条措辞唯一、无漂移风险）。元数是 60 处**同一措辞**重复，漂移即缺陷，故收口进 `arity_error`。判据是「该措辞在库内出现几次、是否应当逐字相同」。
- **码名 + detail 语义重叠保留**（通则 7）。
- **用户函数元数不带函数名**（函数可匿名，归属由 `at` 行给出）；builtins 方法名在 detail 内（方法名不一定进 `at` 行）。
