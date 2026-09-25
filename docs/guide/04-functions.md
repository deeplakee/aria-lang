# 第 4 章 函数

函数是 aria 的一等值：可以存进变量、放进容器、当参数传、当返回值带回。本章讲函数的声明
与调用、lambda、默认参数与 varargs，以及一等值的玩法。函数体引用外层局部变量即成闭包，
语义足够特别，下一章单独讲。

## 声明与调用

`fun name(params) { ... }` 声明命名函数，`return` 返回值；参数与返回都不标类型：

```aria
fun max(a, b) {
    return if (a > b) { a } else { b };
}
println(max(3, 9));
println(max(-1, -5));
```

输出：

```text
9
-1
```

不写 `return` 或写裸 `return;` 的函数返回 `nil` -- 没有「无返回值函数」这个独立种类，
一切函数都返回值：

```aria
fun log(message) {
    println("[log] " + message);
}
var result = log("hi");
println(result);
println(type(log));
```

输出：

```text
[log] hi
nil
Closure
```

`type()` 对函数统一返回 `"Closure"`。提前返回是常态写法：

```aria
fun safe_div(a, b) {
    if (b == 0) { return nil; }
    return a / b;
}
println(safe_div(7, 2));
println(safe_div(1, 0));
```

输出：

```text
3
nil
```

## lambda

表达式位置写 `fun(params) { ... }` 即 lambda。语句位置的 `fun` 后跟名字是声明，后跟 `(` 是
lambda，按位置消歧：

```aria
var square = fun(x) { return x * x; };
println(square(6));

var ops = [fun(x) { return x + 1; }, fun(x) { return x * 2; }];
println(ops[0](10));
println(ops[1](10));
```

输出：

```text
36
11
20
```

`ops[0](10)` 这样的「下标取值再调用」链是合法的：调用、取字段、取下标同属后缀层，
可以任意串联。

## 默认参数

参数可以带默认值，调用时未传则用默认。默认值在**调用发生时**求值，并且可以引用排在它
前面的参数：

```aria
fun greet(name, punct = "!") {
    return "hi " + name + punct;
}
println(greet("aria"));
println(greet("aria", "?"));

fun add_scale(a, b = a * 2) {
    return a + b;
}
println(add_scale(3));
println(add_scale(3, 10));
```

输出：

```text
hi aria!
hi aria?
9
13
```

带默认值的参数必须排在无默认参数之后，否则编译期报错：

<!-- expect-error: DefaultAfterPlain -->
```aria
fun f(a = 1, b) { return a + b; }
```

```text
params.aria:1:15: Syntax: DefaultAfterPlain non-default parameter after default parameter
```

## varargs

`...name` 形式的参数收集多余实参为一个 **list**（每次调用新铸一个），必须排在参数表末尾：

```aria
fun count_all(first, ...rest) {
    var n = 1;
    for (r in rest) { n += 1; }
    return n;
}
println(count_all(1));
println(count_all(1, 2, 3, 4));

fun mixed(a, b = 2, ...c) {
    return a * 100 + b * 10 + c.size();
}
println(mixed(1));
println(mixed(1, 5, 6, 7));
```

输出：

```text
1
4
120
152
```

`mixed(1)` 时 `b` 取默认 2、`c` 是空表：`100 + 20 + 0 = 120`；`mixed(1, 5, 6, 7)` 时
`b` 收 5、`c` 收 `[6, 7]`：`100 + 50 + 2 = 152`。

varargs 不在末尾同样是编译期错误：

<!-- expect-error: VarargsNotLast -->
```aria
fun f(...rest, b) { return rest.size(); }
```

```text
params.aria:1:14: Syntax: VarargsNotLast varargs '...' must be the last parameter
```

调用时实参个数不符是运行期错误（元数检查发生在调用时）：

<!-- expect-error: WrongArity -->
```aria
fun add(a, b) { return a + b; }
println(add(1));
```

```text
Runtime: WrongArity function expects 2 arguments, got 1
  at <main> (arity.aria:3)
```

## 一等函数

函数可以当值传递：当参数、当返回值、存进容器：

```aria
fun apply_twice(f, x) {
    return f(f(x));
}
println(apply_twice(fun(v) { return v * 3; }, 2));

var ops = {"inc": fun(x) { return x + 1; }, "dbl": fun(x) { return x * 2; }};
println(ops["inc"](10));
println(ops["dbl"](10));
```

输出：

```text
18
11
20
```

「函数接收函数」就是回调的全部机制 -- aria 的列表 `sort()` 不带比较回调（第 6 章），
需要自定义排序时，惯用法就是把「key 函数 + sort」包成一个你自己的高阶函数。
返回函数的函数则依赖闭包捕获，下一章展开。

## 递归

函数可以直接调自己。阶乘：

```aria
fun fact(n) {
    return if (n <= 1) { 1 } else { n * fact(n - 1) };
}
println(fact(10));
```

输出：

```text
3628800
```

调用帧栈有上限（256 帧），无终止条件的递归会以 `StackOverflow` 收场：

<!-- expect-error: StackOverflow -->
```aria
fun spin(n) { return spin(n + 1) + 0; }
spin(0);
```

```text
Runtime: StackOverflow call frame stack overflow
  at <main> (spin.aria:2)
  at spin (spin.aria:1)
  at spin (spin.aria:1)
  ...
```

（堆栈跟踪把 256 帧全部打印出来，这里省略中段。）

## 小结

- `fun name(params) { ... }` 声明函数；lambda 是表达式 `fun(params) { ... }`。
- 一切函数都返回值：不写 `return` 得 `nil`；`type()` 返回 `"Closure"`。
- 默认参数在调用时求值、可引用先前参数，必须排在无默认参数之后。
- `...name` varargs 收集多余实参为 list，必须居末。
- 元数不符是运行期 `WrongArity`；递归超 256 帧报 `StackOverflow`。

## 练习

1. 写 `fib(n)` 返回斐波那契数列第 n 项（n 从 0 起），打印 `fib(0)` 到 `fib(10)`。
2. 写 `sum_digits(n)`：循环拆位求整数各位数字之和，`sum_digits(12345)` 应得 15。
3. 写高阶函数 `apply_n(f, x, n)`：把 `f` 连续应用 `n` 次到 `x` 上，用
   `apply_n(fun(v) { return v * 2; }, 1, 10)` 验证得 1024。
4. 写 `fun repeat_str(s, n = 2)`：默认重复两遍返回拼接结果（`"ab"` 与 `n = 3` 得
   `"ababab"`）。

---

[上一章：控制流](03-control-flow.md) · [下一章：闭包](05-closures.md)
