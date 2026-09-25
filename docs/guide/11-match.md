# 第 11 章 match

`match` 按「拿 subject 逐个和模式比较」的方式分流，既能当语句也能当表达式（和 `if` 一样
的双重身份）。但先泼一盆冷水：**aria 的 match 模式不是 Rust / Python 那种绑定模式** -- 
它就是「表达式 + `==` 比较」加一个 `_` 通配。它没有解构模式、没有绑定、没有 `or` 模式、
没有守卫。把它理解成「更整齐的 if-else 链」就不会踩坑。

## 语句形态

`match (subject) { 模式 => 语句 ... }`。每条臂是**一条语句**，自带分号，臂之间没有分隔符；
多语句的臂用块包：

```aria
var n = 2;
match (n) {
    1 => println("one");
    2 => {
        println("two");
        println("dos");
    }
    _ => println("many");
}
```

输出：

```text
two
dos
```

subject 只求值一次，然后从上到下找第一条匹配的臂，其余臂不再看。

## 表达式形态

表达式位置的 match，各臂是**表达式**（没有花括号），臂间以**逗号**分隔，不允许尾逗号：

```aria
var n = 2;
var word = match (n) { 1 => "one", 2 => "two", _ => "many" };
println(word);
println(match (0) { 0 => "zero", 2 => "two", _ => "many" });
```

输出：

```text
two
zero
```

尾逗号是编译错误：

<!-- expect-error: ExpectedExpression -->
```aria
var x = match (1) { 1 => "a", };
```

```text
match.aria:1:31: Syntax: ExpectedExpression expected expression, got '}'
```

两种形态的选择：要「各走各的逻辑」用语句形态，要「按值取值」用表达式形态。

## 模式：表达式按 `==` 匹配

除 `_` 外，每个模式就是一个**普通表达式**：先求值，再和 subject 比 `==`。两个直接推论：

**数值跨类型命中** -- `==` 在数值域跨 int / float 成立，所以 `1.0` 能匹配 `1`：

```aria
println(match (1) { 1.0 => "hit", _ => "miss" });
```

输出：

```text
hit
```

**模式不绑定变量** -- 这是从 Rust / Python 迁移过来最容易错的一点。写一个名字，它不是
「绑定任意值」，而是「拿这个名字的**当前值**来比较」：

```aria
var threshold = 10;
var v = match (5) { threshold => "equal to threshold", _ => "other" };
println(v);
var w = match (10) { threshold => "equal to threshold", _ => "other" };
println(w);
```

输出：

```text
other
equal to threshold
```

`threshold =>` 比较的是 `5 == threshold`（即 `5 == 10`），不成立就落到 `_`。所以「把
subject 存下来供臂内使用」在 aria match 里做不到，需要值就在臂里重新读 subject 变量：

```aria
var code = 3;
match (code) {
    1 => println("first");
    _ => println("code is " + str(code));
}
```

输出：

```text
code is 3
```

模式可以是任意复杂度的表达式（调用、下标都行），只要它能求值出可比较的东西。

## `_` 通配臂

`_` 匹配一切，**只能放最后** -- 放在它后面的臂永远走不到，编译期直接拒绝：

<!-- expect-error: UnreachableArm -->
```aria
var x = match (1) { _ => "any", 1 => "one" };
```

```text
match.aria:1:38: Semantic: UnreachableArm unreachable arm after '_'
```

## 没有匹配时

subject 与所有臂都不相等（且没有 `_`）是**运行期错误** -- match 不做隐式兜底：

<!-- expect-error: MatchNoArm -->
```aria
var x = match (5) { 1 => "a", 2 => "b" };
println(x);
```

```text
Runtime: MatchNoArm no arm matched
  at <main> (match.aria:1)
```

要兜底就显式写 `_` 臂。作为表达式使用时尤其记得这一点：漏了 `_` 的 match 是一颗延迟到
运行期的雷。

## 它擅长什么

模式按 `==` 比较，所以 match 的甜区是「少量精确值 + 兜底」：状态码、命令字、单字枚举：

```aria
fun http_reason(status) {
    return match (status) {
        200 => "OK",
        301 => "Moved Permanently",
        404 => "Not Found",
        500 => "Internal Server Error",
        _ => "Unknown"
    };
}
println(http_reason(404));
println(http_reason(502));
```

输出：

```text
Not Found
Unknown
```

区间、类型判断这类「条件分流」仍然用 if-else 链（aria 没有 `is` / `when` 守卫），match
不抢这个活。

## 小结

- match 双形态：语句臂以分号自结尾、无分隔符；表达式臂以逗号分隔、禁尾逗号。
- 模式 = 表达式按 `==` 比较（数值跨类型可命中）+ `_` 通配；**不绑定变量**。
- `_` 必须居末；无匹配是运行期 `MatchNoArm`，要兜底显式写 `_`。
- 没有 Rust 式解构 / 绑定 / or 模式 / 守卫 -- 它是整齐的 if-else 链。

## 练习

1. 写 `fun day_kind(n)`：1-5 返回 `"weekday"`、6-7 返回 `"weekend"`、其余 `"invalid"`
    -- 注意区间判断用不了 match 模式，想想怎么组织（提示：match 只对精确值，可以用两次
   比较的结果作为 subject）。
2. 用 match 表达式写简易计算器：`calc(op, a, b)` 支持 `"+"`、`"-"`、`"*"`、`"/"`，除零
   返回 `nil`，未知 op 返回 `nil`。
3. 不运行先推理：`match ("1") { 1 => "int", "1" => "string", _ => "other" }` 的值是什么？
   验证并解释（提示：`"1" == 1` 成立吗？）。
4. 把第 3 章 FizzBuzz 里的 if-else 链改写成 match 表达式（提示：subject 用两个整除判断
   拼出的值，比如「能被 3 整除」与「能被 5 整除」的布尔组合）。

---

[上一章：解构](10-destructuring.md) · [下一章：类与对象](12-classes.md)
