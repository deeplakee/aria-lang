# 第 9 章 区间与迭代

区间（`range`）表达一段整数：`1..5`。它没有字面量式的构造函数，就是这两个点运算符；主要
用途两个 -- 被 for-in 遍历、当下标做切片（第 6 / 8 章已经用过）。本章把 range 的形态讲全，
然后落到 aria 的统一迭代协议：for-in 怎么遍历各种容器、怎么显式拿迭代器、以及怎么让自己的
类变得可遍历（入口在[第 12 章](12-classes.md)）。

## 区间的三种形态

| 写法 | 含义 |
| :--- | :--- |
| `a..b` | 闭区间：含两端，`a` 到 `b` |
| `a...b` | 半开区间：含 `a` 不含 `b` |
| `a..` / `a...` | 无上界：从 `a` 起的无限区间（两写法同义） |

只能省上界、不能省下界。端点必须是整数，浮点端点是运行期错误；区间**不可结合**（`a..b..c`
不合法）：

<!-- expect-error: TypeMismatch -->
```aria
var r = 1.5..3;
```

```text
Runtime: TypeMismatch range bounds must be integers, got F64 and Int
  at <main> (range.aria:1)
```

<!-- expect-error: ExpectedToken -->
```aria
var r = 1..2..3;
```

```text
range.aria:1:13: Syntax: ExpectedToken expected ';', got '..'
```

range 是不可变值：`type(1..3)` 是 `"Range"`，打印成 `1..3`。它的方法面目前只有 `iter()`。

## for-in：遍历区间

```aria
for (i in 1..5) {
    println(i);
}
for (i in 0...3) {
    println(i);
}
```

输出：

```text
1
2
3
4
5
0
1
2
```

**方向由端点大小决定**：`from > to` 就倒序产出；两端的区间只剩空区间一种：

```aria
for (i in 3..1) { println(i); }   # 倒序：3 2 1
for (i in 1...1) { println(i); }  # 空：半开 a...a
for (i in 5..5) { println(i); }   # 只含 5：闭区间 a..a
```

输出：

```text
3
2
1
5
```

无上界区间无限迭代（`for (i in 1..)` 不会自己停），必须靠 `break` 退出 -- 配合条件做
「从 1 数到满足条件为止」的循环很自然：

```aria
var n = 1;
for (i in 1..) {
    n *= 2;
    if (n >= 100) { break; }
}
println(n);
```

输出：

```text
128
```

（从 Python 来的读者注意：aria 的 `a..b` 是**闭**区间，`range(1, 5)` 对应 `1...5`。）

## for-in：遍历各容器

被遍历的对象先交出迭代器，每轮从迭代器取下一个值。各容器产出什么：

| 源 | 每轮产出 |
| :--- | :--- |
| list | 元素 |
| string | 字符（码点域，一个字符一个字符串） |
| map | `[key, value]` 二元列表（顺序未规定） |
| range | 整数 |
| 用户类实例 | 由类的 `next()` 决定（第 12 章） |

```aria
for (c in "héy") { println(c); }

var m = {"a": 1, "b": 2};
for ([k, v] in m) { println(k + "=" + str(v)); }
```

输出：

```text
h
é
y
a=1
b=2
```

（map 只有两键时顺序碰巧稳定，但别依赖 -- 正式口径见第 7 章。）

循环目标除了名字，还可以是 `_`（丢弃每轮的值）或解构模式（第 10 章）：

```aria
var total = 0;
for (_ in 1..100) { total += 1; }
println(total);
```

输出：

```text
100
```

## 显式迭代器

每个可遍历类型都有 `iter()` 方法，返回显式迭代器，其上只有 `has_next()` 与 `next()`：

```aria
var it = [10, 20, 30].iter();
println(it.has_next());
println(it.next());
println(it.next());
println(it.next());
println(it.has_next());
```

输出：

```text
true
10
20
30
false
```

两次 `iter()` 拿到的迭代器游标彼此独立：

```aria
var it1 = [10, 20, 30].iter();
var it2 = [10, 20, 30].iter();
it1.next();
it1.next();
it1.next();
println(it1.has_next());
println(it2.has_next());
```

输出：

```text
false
true
```

`next()` 在耗尽后再调是运行期错误（fail-fast，不返哨兵值）：

<!-- expect-error: IterationExhausted -->
```aria
var it = [1].iter();
it.next();
it.next();
```

```text
Runtime: IterationExhausted iterator exhausted
  at <main> (iter.aria:3)
```

## 迭代协议

for-in 没有魔法，它就是上面三件事的语法糖，对**任何**被遍历对象都这样降：

```text
隐藏局部 it = 源.iter()
循环：it.has_next() 为假则退出（continue 也回到这里判断）
每轮（新作用域）：模式 = it.next()
```

所以「让一个东西可遍历」等价于「让它有 `iter()`，且返回的东西有 `has_next()` / `next()`」。
内置类型全走这条路；自己的类怎么接入见[第 12 章](12-classes.md)的自定义可迭代一节。
顺带一提：`it.next` 这种「方法本身」也是一等值，可以取出来放进变量再调用 -- 第 12 章的
绑定方法会解释这套机制。

## 小结

- `a..b` 闭、`a...b` 半开、`a..` 无上界；端点须整数；`from > to` 倒序；不可结合。
- for-in 的产出：list 元素、string 字符、map `[k, v]`、range 整数。
- 显式迭代器 `iter()` / `has_next()` / `next()`，游标独立，耗尽后再 `next()` 报错。
- 协议就三个方法，for-in 是它的语法糖。

## 练习

1. 用一行 range 求 1 到 100 的整数和（for-in 累加）。
2. 倒着打印 10 到 1，再用「字符串切片 + 倒序 range」的思路写 `reverse(s)`：
   `reverse("hello")` 应得 `"olleh"`（提示：区间端点可以用表达式）。
3. 写 `zip_sum(a, b)`：两个等长列表逐位相加，返回新列表 -- **不许用下标**，用两个显式
   迭代器同步推进。
4. 不运行先推理：`for (i in 5...5) { println(i); }` 与 `for (i in 5..5) { println(i); }`
   各打印什么？验证后用一句话说明半开与闭区间在「两端相等」时的差别。

---

[上一章：字符串](08-strings.md) · [下一章：解构](10-destructuring.md)
