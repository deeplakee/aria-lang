# 第 3 章 控制流

aria 的控制流件数不多但都齐全：`if` / `else`（语句与表达式两种身份）、`while`、C 风格三段
`for`、`for-in`、`break` / `continue`，以及第 11 章的 `match`。本章讲前三类与循环控制；
`for-in` 涉及迭代协议，放到第 9 章与区间一起讲。

## if 语句

条件必须带括号，体可以是单条语句或块。`else` 可选，`else if` 链直接写：

```aria
var grade = 87;
if (grade >= 90) {
    println("A");
} else if (grade >= 80) {
    println("B");
} else {
    println("C");
}
```

输出：

```text
B
```

体为单条语句时可以不套块：

```aria
var n = 42;
if (n > 0) println("positive"); else println("non-positive");
```

条件位置的真假判定沿用第 2 章的规则：只有 `false` 与 `nil` 为假。没有 `elif` 关键字，
没有三元 `?:` 运算符（要按条件取值请用下面的 if 表达式）。

## if 表达式

`if` 同时是表达式：条件为真取 then 分支的值，否则取 else 分支的值。**两个分支都必须带
花括号，且花括号内是单个表达式**：

```aria
var grade = 87;
var level = if (grade >= 90) { "A" } else { "B" };
println(level);
```

输出：

```text
B
```

分支可以嵌套 if 表达式（内层整体是一个表达式）：

```aria
var n = 0;
var sign = if (n > 0) { 1 } else { if (n < 0) { -1 } else { 0 } };
println(sign);
```

输出：

```text
0
```

花括号里放语句（比如先声明再求值）是编译错误 -- 表达式位置只收表达式：

<!-- expect-error: ExpectedExpression -->
```aria
var t = 1;
var v = if (t == 1) { var a = 10; a + 1 } else { 0 };
```

```text
ifexpr.aria:2:23: Syntax: ExpectedExpression expected expression, got 'var'
```

需要多步计算的值，写成语句形式先赋值，或者抽成函数（第 4 章）：

```aria
var t = 1;
var v = 0;
if (t == 1) {
    var a = 10;
    v = a + 1;
}
println(v);
```

输出：

```text
11
```

## while

`while (cond) statement`，条件为假即退出。经典倒累加：

```aria
var n = 5;
var acc = 0;
while (n > 0) {
    acc += n;
    n -= 1;
}
println(acc);
```

输出：

```text
15
```

Collatz 步数是 `while` 的标准用例（条件随循环体变化，段数事先未知）：

```aria
var n = 27;
var steps = 0;
while (n != 1) {
    n = if (n % 2 == 0) { n / 2 } else { 3 * n + 1 };
    steps += 1;
}
println(steps);
```

输出：

```text
111
```

## C 风格 for

三段式 `for (init; cond; step) statement`：init 是 `var` 声明、表达式语句或空，cond 与
step 都可省（省 cond 即恒真）：

```aria
var total = 0;
for (var i = 1; i <= 5; i += 1) {
    total += i;
}
println(total);
```

输出：

```text
15
```

step 留空、循环体内自己推进：

```aria
for (var i = 0; i < 3;) {
    println(i);
    i += 2;
}
```

输出：

```text
0
2
```

`for (;;)` 是无限循环，配合 `break` 使用：

```aria
var i = 0;
for (;;) {
    i += 1;
    if (i == 3) { break; }
}
println(i);
```

输出：

```text
3
```

aria 没有后缀 `i++`，循环步进惯用 `i += 1` 或 `++i`（见第 2 章一元运算符）。

## break 与 continue

`break` 跳出最内层循环，`continue` 进入下一轮。用在循环外是编译期错误：

```aria
for (i in 1..10) {
    if (i % 2 == 0) { continue; }
    if (i > 7) { break; }
    println(i);
}
```

输出：

```text
1
3
5
7
```

<!-- expect-error: BreakOutsideLoop -->
```aria
fun f() { break; }
```

```text
loop.aria:1:11: Semantic: BreakOutsideLoop 'break' outside loop
```

（这里 `for-in` 提前登场了，语法见第 9 章。）

## 块作用域

块 `{ ... }` 引入新作用域，块内 `var` 出块即不可见：

```aria
var x = 1;
{
    var y = 2;
    println(x + y);
}
println(x);
```

输出：

```text
3
1
```

出块后再读块内名字，按「未定义名字」报运行期错误：

<!-- expect-error: UndefinedVariable -->
```aria
{
    var y = 2;
}
println(y);
```

```text
Runtime: UndefinedVariable undefined global 'y'
  at <main> (scope.aria:4)
```

## for-in 一览

遍历容器的日常形态是 `for (pattern in iterable) statement`，list 逐元素、string 逐码点、
map 逐键值对、range 逐整数：

```aria
var names = ["ada", "grace", "linus"];
for (name in names) {
    println("hi, " + name);
}
```

输出：

```text
hi, ada
hi, grace
hi, linus
```

它背后是统一的迭代协议（`iter()` / `has_next()` / `next()` 三个方法），细节、倒序遍历、
自定义可迭代类都在[第 9 章](09-ranges-and-iteration.md)。

## 小结

- `if` 有语句与表达式两种身份；表达式形态两分支都强制花括号、内放单个表达式，`else` 必写。
- `while` 适合段数未知的循环；三段 `for` 的 init / cond / step 都可省，`for (;;)` 无限。
- `break` / `continue` 只作用于最内层循环，循环外使用是编译错误。
- 块 `{}` 引入作用域，块内声明出块即失效。
- 条件位置只有 `false` 与 `nil` 为假。

## 练习

1. FizzBuzz：打印 1 到 30，能被 3 整除打 `Fizz`、被 5 整除打 `Buzz`、都整除打 `FizzBuzz`，
   其余打数字。用 `if / else if / else` 实现。
2. 用 `while` 求 1000 以内最大的 7 的倍数。
3. 写嵌套三段 `for` 打印九九乘法表的一行（如 `3 x 4 = 12`，只打 3 的那一行）。
4. 用 if 表达式写 `clamp(x, lo, hi)`：把 `x` 限制在 `[lo, hi]` 内（不需要函数语法，
   直接对 `x = 42, lo = 0, hi = 10` 算出结果并打印）。

---

[上一章：值与变量](02-values-and-variables.md) · [下一章：函数](04-functions.md)
