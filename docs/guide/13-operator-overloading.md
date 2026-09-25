# 第 13 章 运算符重载

在类上定义特定名字的方法，实例就能出现在运算符两侧、被当函数调用。aria 开放了 11 个
钩子：五个算术、四个比较、一元取负、调用。规则干净：**只有左操作数 / 被调用对象的类被
咨询**，没有 Python 那套 `__radd__` 反射形态；钩子就是普通方法，按第 12 章的成员查找
规则取实现。

## 钩子全表

| 运算 | 钩子 | 触发形态 |
| :--- | :--- | :--- |
| `a + b` | `__add__(b)` | 算术 |
| `a - b` | `__sub__(b)` | 算术 |
| `a * b` | `__mul__(b)` | 算术 |
| `a / b` | `__div__(b)` | 算术（`/` 是唯一的除法算子） |
| `a % b` | `__mod__(b)` | 算术 |
| `a < b` | `__lt__(b)` | 比较 |
| `a <= b` | `__le__(b)` | 比较 |
| `a > b` | `__gt__(b)` | 比较 |
| `a >= b` | `__ge__(b)` | 比较 |
| `-a` | `__neg__()` | 一元取负 |
| `a(args)` | `__call__(args)` | 调用 |

**不参与重载**：`==` / `!=`（永远是内容相等语义，第 2 章）与下标 `[]`。aria 没有
`__eq__`、`__index__` 这类口子。

## 一个完整例子：Vec2

```aria
def Vec2 {
    init(x, y) {
        this.x = x;
        this.y = y;
    }
    __add__(other) { return Vec2(this.x + other.x, this.y + other.y); }
    __sub__(other) { return Vec2(this.x - other.x, this.y - other.y); }
    __neg__() { return Vec2(-this.x, -this.y); }
    __lt__(other) {
        return this.x * this.x + this.y * this.y < other.x * other.x + other.y * other.y;
    }
    show() { return "Vec2(" + str(this.x) + ", " + str(this.y) + ")"; }
}
var u = Vec2(1, 2);
var w = Vec2(3, 4);
println((u + w).show());
println((-u).show());
println((w - u).show());
println(u < w);
```

输出：

```text
Vec2(4, 6)
Vec2(-1, -2)
Vec2(2, 2)
true
```

钩子方法就是普通实例方法：参数照常收右操作数、`this` 是左操作数、可以带默认参数、
返回值由你定义（不必返回同类，`__lt__` 返回布尔、`__add__` 也可以返回别的东西 -- 
但让 `+` 返回一个 Vec2 的读者预期最不容易落空）。

`show()` 这种渲染辅助方法体现了 aria 的一个直白现实：没有 `__str__` 钩子，`println(vec)`
打印的是实例的默认形态，想要人读的样子就自己写 `show()` 并显式调用。

## `__call__`：让实例像函数

```aria
def Multiplier {
    init(n) { this.n = n; }
    __call__(x) { return x * this.n; }
}
var triple = Multiplier(3);
println(triple(5));
```

输出：

```text
15
```

「携带配置的可调用对象」是它的定位：闭包能做的事它都能做，还多了类的一整套字段与方法。
`type(triple)` 仍是 `"Instance"` -- 它不是函数，只是装作函数。

## 触发规则：只看左边

`obj + 1` 咨询 obj 的类；`1 + obj` 不咨询任何钩子 -- 左操作数是 Int，落进数值域，类型
不匹配直接报错：

<!-- expect-error: TypeMismatch -->
```aria
def Box { init(v) { this.v = v; } }
var b = Box(1);
var r = 1 + b;
```

```text
Runtime: TypeMismatch operator '+' requires numbers, got Int and Instance
  at <main> (op.aria:3)
```

想让两个方向都成立，就得保证**两个操作数都是你的类型**（`Vec2 + Vec2`），或者把非对象
一侧包进去。从 Python 过来的读者请把 `__radd__` 从心里删掉。

实例上没定义钩子则是「成员缺失」：

<!-- expect-error: UndefinedProperty -->
```aria
def Box { init(v) { this.v = v; } }
var b = Box(1);
var r = b + 1;
```

```text
Runtime: UndefinedProperty <class Box> has no member '__add__'
  at <main> (op.aria:3)
```

## 内置类型走的同一通道

字符串的 `+` 与 `<` 就是 String 类上的内建 `__add__` / `__lt__` -- 第 2 章见过的
`"n=" + 42` 报错（`__add__ requires two strings, got String and Int`）正是 String 的
`__add__` 在抱怨右操作数。内置类型与用户类没有两套运算机制：list 不支持 `+`，不是
「运算符碰巧没实现」，而是 List 类上没有 `__add__` 这个成员（`type List does not
support '__add__'`）。统一通道也意味着：给自己的类定义 `__add__` 后，它与字符串等内置
类型在运算层面平起平坐。

## 小结

- 11 个钩子：`__add__ __sub__ __mul__ __div__ __mod__`、`__lt__ __le__ __gt__ __ge__`、
  `__neg__`、`__call__`；`==` 与 `[]` 不参与重载。
- 钩子是普通方法，仅左操作数 / 被调用对象触发，无反射形态。
- 缺钩子报成员缺失（`has no member '__add__'`），左操作数是内置类型时走各算子自己的
  域检查（`operator '+' requires numbers`）。
- 没有 `__str__`：打印形态自己写 `show()` 一类的方法。

## 练习

1. 给第 12 章练习的 `Vec2` 补上 `__mul__`（数乘：`__mul__(k)` 里 k 是数值）与 `__le__`，
   验证 `Vec2(1, 1) * 3` 与 `Vec2(1, 1) <= Vec2(2, 2)`。
2. 写 `Money` 类：内部用整数存「分」，`__add__` 拒绝与普通数值相加（直接 `throw`，
   第 14 章预告），`__lt__` 按金额比较，`show()` 渲染 `"12.34"` 形态。
3. 写 `Predicate` 类：构造收一个函数 `f`，`__call__(x)` 返回 `f(x)`；再定义
   `__neg__`？ -- 先想清楚 aria 的 `!` 是不是钩子（提示：不是，`!` 永远做逻辑非），
   然后用别的方式实现「反谓词」（比如返回一个新 Predicate）。
4. 不运行先推理：`def C { __lt__(o) { return true; } }`，两个实例 `c1 < c2` 与
   `c2 < c1` 各是什么？这暴露了钩子机制的什么性质？（提示：你没有义务让比较满足
   严格弱序，但违反它的代价是什么？）

---

[上一章：类与对象](12-classes.md) · [下一章：异常](14-exceptions.md)
