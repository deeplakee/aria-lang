# 第 2 章 值与变量

aria 是动态类型语言：变量不标类型，值携带类型。本章过一遍全部基础值类型（`nil` / `bool` /
`int` / `float` / `string`）、字面量写法、`var` 声明与赋值语义，以及运算符的全集和行为细节。
容器（`list` / `map` / `range`）与函数、类分别在后续章节展开。

贯穿全章的一条主线：**aria 不做隐式类型转换**。数值与字符串混算不会自动转，相等比较分
`==`（内容）与 `===`（严格）两级。从 Python / JavaScript 过来的读者请特别留意差异点标注。

## 动态类型与 type()

内建函数 `type()` 返回值的类型名字符串：

```aria
println(type(1));       # Int
println(type(1.5));     # F64
println(type("aria"));  # String
println(type(true));    # Bool
println(type(nil));     # Nil
println(type([1, 2]));  # List
println(type({}));      # Map
println(type(1..3));    # Range
```

输出：

```text
Int
F64
String
Bool
Nil
List
Map
Range
```

浮点类型的名字是 `F64`（对应 C 的 `double`）。函数、闭包、类、实例、模块、迭代器也是值，
`type()` 分别返回 `"Closure"`、`"Class"`、`"Instance"`、`"Module"`、`"Iterator"`，
见面时再细说。

## nil 与 bool

`nil` 表示「没有值」，`true` / `false` 是布尔值。三者都是字面量关键字（注意是 `nil`，
不是 `null`）。

条件位置的真假判定规则要记牢：**只有 `false` 和 `nil` 为假，其余一切都为真** -- 包括 `0`、
`""`、空列表。这与 Python / JavaScript 正相反：

```aria
fun truthy(v) {
    return if (v) { "truthy" } else { "falsy" };
}
println(truthy(false));  # falsy
println(truthy(nil));    # falsy
println(truthy(0));      # truthy（0 不是假值）
println(truthy(""));     # truthy（空串不是假值）
println(truthy([]));     # truthy（空列表不是假值）
```

输出：

```text
falsy
falsy
truthy
truthy
truthy
```

（`if (...) { ... } else { ... }` 作表达式取值，第 3 章细讲。）

## int：48 位整数

aria 的整数是 48 位有符号整数，值域 `[-2^47, 2^47)`，即 `[-140737488355328, 140737488355327]`。
支持二 / 八 / 十六进制与数字间的 `_` 分隔（`_` 只能出现在数字之间）：

```aria
println(255);        # 十进制
println(0b11111111); # 二进制
println(0o377);      # 八进制
println(0xFF);       # 十六进制
println(1_000_000);  # 下划线分隔，仅作可读性
println(140737488355327);  # 值域上界
```

输出：

```text
255
255
255
255
1000000
140737488355327
```

超出值域的字面量在编译期直接报错：

<!-- expect-error: NumberOutOfRange -->
```aria
var x = 140737488355328;
```

```text
lit.aria:1:9: Semantic: NumberOutOfRange integer literal 140737488355328 out of range
```

## float：双精度浮点

`float` 是 IEEE 754 双精度（`f64`）。字面量要么带小数点，要么带指数；**必须是一个完整形态**，
`.5` 与 `5.` 都不合法：

```aria
println(1.5);
println(1e3);      # 指数形态，1000.0
println(1.5e2);    # 150.0
println(1.0 / 3);  # 0.3333333333333333
```

输出：

```text
1.5
1000.0
150.0
0.3333333333333333
```

<!-- expect-error: ExpectedExpression -->
```aria
var half = .5;
```

```text
lit.aria:1:12: Syntax: ExpectedExpression expected expression, got '.'
```

`.5` 里的 `.` 会按取字段运算符解析，于是表达式在 `.` 处断掉。要写 `0.5`。

整数与浮点混合运算时升为浮点：`1 + 2.5` 得 `3.5`。浮点运算不检查除零：除以 `0`（整数）
才是错误，除以 `0.0`（浮点）按 IEEE 规则得无穷或 NaN：

```aria
println(1.0 / 0);    # inf
println(0.0 / 0.0);  # nan
```

<!-- expect-error: DivisionByZero -->
```aria
var x = 1 / 0;
```

```text
Runtime: DivisionByZero integer division by zero
  at <main> (div.aria:1)
```

## string 字面量

字符串是 UTF-8 字节序列，字面量用双引号或单引号包住（首尾须一致），**不能跨行**。转义表：

| 转义 | 含义 |
| :--- | :--- |
| `\n` `\t` `\r` `\0` | 换行、制表、回车、NUL |
| `\\` | 反斜杠本身 |
| `\"` `\'` | 引号（在哪种引号里都要转义也可以直接写另一种） |
| `\u{...}` | Unicode 码点，如 `\u{1F600}`，`{}` 内是十六进制 |
| `\$` | 字面 `$`（写字面 `${` 用，见下文插值） |

未列出的转义（比如 `\q`）是编译错误；没有 `\x` 形态。

```aria
println("double quoted");
println('single quoted');
println("line1\nline2");
println("tab:\there");
println("emoji: \u{1F600}");
println("quote: \" backslash: \\");
```

输出：

```text
double quoted
single quoted
line1
line2
tab:	here
emoji: 😀
quote: " backslash: \
```

字符串自带插值：`${表达式}` 档内是完整表达式，值按 `str()` 的可读渲染嵌进结果 --
`"n = ${n}"` 与 `"n = " + str(n)` 等价（嵌套上限与 `\$` 转义见第 8 章「插值」）。
`f"..."` 不作特殊处理（`f` 是普通标识符）；`+` 拼接仍然只在两侧都是字符串时成立。

```aria
var n = 42;
println("n = ${n}");
```

字符串的方法面（`split` / `trim` / `to_int` 等）在第 8 章完整展开。

## var：声明与赋值

aria 只有一种声明形式 `var`，没有 `let`，也就没有不可变绑定 -- 一切 `var` 都可再赋值。
一条 `var` 可以声明多个绑定；不带初始化的绑定值为 `nil`：

```aria
var a = 1;
var b = 2, c = 3;
var d;
println(a + b + c);  # 6
println(d);          # nil
```

**赋值不创建变量**：给未声明的名字赋值是运行期错误，必须先 `var` 声明：

<!-- expect-error: UndefinedVariable -->
```aria
missing = 1;
```

```text
Runtime: UndefinedVariable undefined global 'missing'
  at <main> (assign.aria:1)
```

赋值本身是表达式，值为右侧的值，因此可以链式也可以塞进更大的表达式：

```aria
var a = 1;
var b = (a = 2);
println(a);  # 2
println(b);  # 2
```

作用域按块分：函数体内的 `var` 是局部，块 `{ ... }` 内的声明出了块就不可见（第 3 章），
模块顶层的 `var` 是模块全局（第 15 章）。

## 运算符

优先级从低到高：**赋值 < `||` < `&&` < 相等 < 比较 < 区间 < 加减 < 乘除模 < 一元 < 后缀**
（调用 / 取字段 / 取下标）。同层多为左结合，一元与赋值右结合。

### 算术：`+` `-` `*` `/` `%`

数值域内运算，`+` 与 `*` 有内建例外：`+` 两侧都是字符串时拼接出新串；`*` 左侧是字符串、
右侧是整数时按次数重复出新串（列表的同款 `+`/`*` 见第 6 章）。**没有其他隐式转换** -- 
字符串加数字直接报错：

<!-- expect-error: TypeMismatch -->
```aria
var s = "n=" + 42;
```

```text
Runtime: TypeMismatch __add__ requires two strings, got String and Int
  at <main> (concat.aria:1)
```

字符串重复的乘数必须是整数（浮点一律报错，负数也报错而不静默得空串）：

```aria
println("ab" * 3);      # ababab
println("ab" * 1);      # ab
println("ab" * 0);      #（空串）
```

整数 `/` 是向零截断的整除，`%` 的符号跟随被除数（与 C 一致）：

```aria
println(7 / 2);    # 3
println(-7 / 2);   # -3
println(7 % 3);    # 1
println(-7 % 3);   # -1
println(7 % -3);   # 1
println(7 / 2.0);  # 3.5（混入浮点则按浮点算）
println(2.5 % 2);  # 0.5
```

输出：

```text
3
-3
1
-1
1
3.5
0.5
```

「商余恒等式」始终成立：`(a / b) * b + a % b == a`。

没有位运算符（`&` `|` `^` `<<` 都不存在），也没有 `**` 幂运算。

### 相等：`==` `!=` 与 `===` `!==`

aria 把相等分成两级：

- `==` **内容相等**：数值跨 `int` / `float` 成立（`1 == 1.0`）；列表、映射按内容递归比较；
  字符串按字节内容比较。
- `===` **严格相等**：类型严格一致（`1 === 1.0` 为假），浮点按位，对象按身份（同一个对象）。

```aria
println(1 == 1.0);         # true（数值内容相等）
println(1 === 1.0);        # false（严格：Int 与 F64 不同类型）
println(true == 1);        # false（bool 与数值没有跨类型相等）
println(nil == nil);       # true
println([1, 2] == [1, 2]); # true（列表按内容）
var xs = [1];
var ys = [1];
println(xs == ys);         # true（两个不同对象，内容相同）
println(xs === ys);        # false（不是同一个对象）
println(xs === xs);        # true
```

输出：

```text
true
false
false
true
true
true
false
true
```

字符串经驻留池处理，相同内容的字符串字面量就是同一个对象，所以 `"a" === "a"` 成立；
但依赖这一点是坏习惯，判断内容请统一用 `==`。`===` 的正经用途是区分内容相等的可变对象
（第 7 章的 map 键语义会大量用到）。

### 比较：`>` `>=` `<` `<=`

比较只定义在两个域上：**数值**（int/float 混合可比）与**字符串**（按无符号字节序，大小写
敏感，没有 locale 排序）。两个域不能混：

<!-- expect-error: TypeMismatch -->
```aria
println(1 < "a");
```

```text
Runtime: TypeMismatch operator '<' requires numbers, got Int and String
  at <main> (cmp.aria:1)
```

```aria
println(1 < 1.5);           # true（数值跨类型可比）
println("apple" < "banana"); # true（字典序）
println("Z" < "a");          # true（字节序：大写 90 < 小写 97）
```

### 逻辑：`||` `&&` 与短路

`||` 与 `&&` 短路求值，并且**返回操作数的值**而不是布尔（同 JavaScript / Lua）：
`a || b` 在 `a` 为真时返回 `a`，否则求值并返回 `b`：

```aria
println(nil || "default");   # default（nil 为假，取 b）
println(0 || "fallback");    # fallback？不 -- 0 为真，结果是 0
println(true && "yes");      # yes（a 为真，取 b）
println(false && 1);         # false（a 为假，短路返回 a）
```

输出：

```text
default
0
yes
false
```

注释里那个反问正是 `0` 为真值的体现。用 `||` 提供默认值时，只有 `nil` / `false` 会触发
兜底，空串、`0` 都不会。

### 一元：`-` `!` 与前置 `++` `--`

`-` 取负，`!` 逻辑非（返回 `true` / `false`）。自增自减**只有前置形态**，且是表达式：

```aria
var i = 0;
println(++i);  # 1（先加后用，表达式值为新值）
var n = 5;
--n;
println(n);    # 4
```

没有后缀 `i++`。写惯 `for (i = 0; i < n; i++)` 的读者请改用 `i += 1` 或 `++i`。

### 序列：逗号

逗号能把多个表达式连成一个**序列表达式**：从左到右逐个求值，整个序列的值是**最后
一个**表达式的值。它是优先级最低的运算符（比赋值还低），元素位可以直接写赋值：

```aria
var log = "";
var note = fun (tag, ret) { log = log + tag; return ret; };
var v = (note("a", 1), note("b", 2));
println(v);      # 2
println(log);    # ab
```

输出：

```text
2
ab
```

序列最常配合三段 `for` 使用（第 3 章）：init 与增量位各容许多个表达式。注意两处
逗号的区别：`var i = 0, j = 9` 是 var 的多绑定声明（逗号是分隔符），`++i, --j` 才是
序列。

`(a, b)` 产出的是序列值，不是元组：没有独立的"元组类型"，值就是最后一个元素。
两处易混的边界：

- 实参列表、列表与 map 字面量、`var` 多绑定里的逗号都是**分隔符**；想把序列值传给
  函数，须再包一层括号：`f((a, b))`。
- `return a, b` 与 `throw a, b` 不支持（直接语法错）--return 的多返回值用 `return [a, b]`
  配解构接收（第 10 章）；throw 要序列语义显式加括号 `throw (a, b)`。

## str() 与 println()

`str()` 把任意值转成可读字符串（与 `println` 的渲染同一套）：`str(3.0)` 得 `"3.0"`，
`str(nil)` 得 `"nil"`，`str([1, 2])` 得 `"[1, 2]"`。`println(x)` 等价于打印 `str(x)`
加换行，省参时只输出换行。两者与 `type()`、`assert()`、`clock()`、`Error()` 一起构成 aria
的全部六个内建函数（另有内建模块 `coroutine`，第 16 章；附录有完整参考），它们都是一等值，
可以传给别的函数。

## 常见错误速查

| 写法 | 报错 |
| :--- | :--- |
| `missing = 1;`（未声明就赋值） | `Runtime: UndefinedVariable undefined global 'missing'` |
| `"n=" + 42` | `Runtime: TypeMismatch __add__ requires two strings, got String and Int` |
| `"ab" * 2.5`（乘数须整数） | `Runtime: TypeMismatch __mul__ requires a string and an integer, got String and F64` |
| `[1] + 2`（拼接两侧须皆列表） | `Runtime: TypeMismatch __add__ requires two lists, got List and Int` |
| `1 / 0`（整数除零） | `Runtime: DivisionByZero integer division by zero` |
| `1 < "a"` | `Runtime: TypeMismatch operator '<' requires numbers, got Int and String` |
| `var x = 140737488355328;` | `Semantic: NumberOutOfRange ...`（编译期） |
| `var x = .5;` | `Syntax: ExpectedExpression expected expression, got '.'`（编译期） |
| `return 1, 2;` | `Syntax: ExpectedToken expected ';', got ','`（编译期；return 不收逗号序列，多返回值用 `return [a, b]`） |
| `throw "bad", "worse";` | `Syntax: ExpectedToken expected ';', got ','`（编译期；throw 不收逗号序列，要序列写 `throw (a, b)`） |

## 小结

- 动态类型，`type()` 查类型名（浮点是 `F64`）。
- 只有 `false` 与 `nil` 为假；`0`、`""`、`[]` 都是真。
- `int` 是 48 位，二 / 八 / 十六进制字面量（`0b`/`0o`/`0x`）+ `_` 分隔；`float` 字面量必须完整（`.5` 非法）。
- 字符串不能跨行；`${}` 插值按 `str()` 渲染嵌值，`+` 拼接须两侧皆字符串。
- 只有 `var`；赋值不创建变量；赋值是表达式。
- 相等分 `==`（内容）与 `===`（严格）；比较限数值与字符串两域；`||`/`&&` 返回操作数值。
- 一元自增自减只有前置 `++i` / `--i`。
- 逗号序列优先级最低：逐个求值、值取最后一个；`(a, b)` 是序列值不是元组。

## 练习

1. 写一个表达式判断某整数「既能被 3 整除又能被 5 整除」，在 REPL 里对 15、30、31 验证。
2. `var a = 1, b = 2;` 用一行解构赋值交换 `a` 与 `b`（第 10 章预告：`[a, b] = [b, a];`），
   现在先用 `var t = a;` 三行版实现。
3. 预测 `0.1 + 0.2 == 0.3` 的结果并验证；再想一个用「差值小于某个小量」判断浮点相等的写法。
4. `println("" || 42);` 输出什么？先推理再验证（提示：空串是真值）。

---

[上一章：开始](01-getting-started.md) · [下一章：控制流](03-control-flow.md)
