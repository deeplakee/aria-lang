# 第 12 章 类与对象

aria 的类用 `def` 定义（不是 `class`）：`init` 是构造器，`this` 指实例，`def Child : Parent`
单继承。成员分三类 -- 实例方法、静态方法、静态变量，靠「写不写 `fun` 关键字」区分。实例
字段是动态的，不需要声明。所有用户类隐式继承内置根类 `Object`。类必须定义在模块顶层
（类体里不能再定义类）。

## 定义、构造、实例化

```aria
def Greeter {
    init(name) {
        this.name = name;
    }
    greet(loud = false) {
        var msg = "hello, " + this.name;
        return if (loud) { msg.upper() + "!" } else { msg + "!" };
    }
}
var g = Greeter("aria");
println(g.greet());
println(g.greet(true));
println(type(g));
println(type(Greeter));
```

输出：

```text
hello, aria!
HELLO, ARIA!
Instance
Class
```

- `init` 是普通实例方法，`ClassName(args)` 实例化时被调。不写 `init` 则继承 `Object` 的
  默认空实现（什么都不做）。
- 实例字段**动态**：`this.name = name` 这一句既是「创建」也是「赋值」，不需要提前声明。
- 方法可以带默认参数与 varargs（第 4 章语法原样适用，`this` 占槽 0 不参与收集）。
- `type()` 对一切实例返回 `"Instance"`、对类本身返回 `"Class"`，不区分类名。

## this：沿外围就近捕获

`this` 只在实例方法里有意义。嵌套在实例方法里的 lambda 能看见外围方法的 `this`：

```aria
def Box {
    init(v) { this.v = v; }
    make_adder() {
        return fun(x) { return this.v + x; };
    }
}
var b = Box(10);
var add = b.make_adder();
println(add(5));
```

输出：

```text
15
```

静态方法与模块顶层没有 `this`，在那里用 `this` 是编译错误。

## 三类成员

`def` 体的成员按起首 token 分派：

| 写法 | 成员 | 归属 | this |
| :--- | :--- | :--- | :--- |
| `name(params) { ... }`（无 `fun`） | 实例方法 | 实例 | 有 |
| `fun name(params) { ... }` | 静态方法 | 类本身 | 无 |
| `var name = expr;` | 静态变量 | 类本身 | -- |

```aria
def Counter {
    var created = 0;          # 静态变量：属类，所有实例共享
    init() {
        Counter.created += 1;
        this.id = Counter.created;
    }
    describe() {              # 实例方法
        return "counter #" + str(this.id);
    }
    fun total() {             # 静态方法：像挂在类上的普通函数
        return Counter.created;
    }
}
var a = Counter();
var b = Counter();
println(Counter.total());
println(a.describe());
println(b.describe());
```

输出：

```text
2
counter #1
counter #2
```

静态变量在**类定义执行时**初始化（eager），静态方法里只能经 `类名.名字` 访问静态与
自身（没有 `this`）。

**字段 / 成员的读取链**：`obj.x` 先查实例字段，未命中沿类链查静态成员；实例字段可以
遮蔽同名静态：

```aria
def Shape {
    var unit = "cm";
    init() {
        this.unit = "m";
    }
    unit_name() { return this.unit; }
}
var s = Shape();
println(s.unit_name());
println(Shape.unit);
```

输出：

```text
m
cm
```

静态成员可以事后动态添加（`Foo.anything = v;` 直接写上就是）。跨类访问都是
`类名.名字` / `实例.名字`，没有 import 之外的可见性修饰。

## 绑定方法

`obj.method` 不带括号取出来，得到**绑定方法** -- `this` 已经绑在那个实例上，可以当普通
函数传递：

```aria
def Greeter {
    init(name) { this.name = name; }
    greet() { return "hi " + this.name; }
}
var g = Greeter("aria");
var m = g.greet;
println(m());
```

输出：

```text
hi aria
```

这正是第 9 章 `it.next` 能取出来用的原因 -- 方法值与闭包一样是一等公民。注意绑定是
**每次访问现场发生**的：先取 `g.greet` 再改 `g` 的字段，调用时用的是新值。

## 继承与 super

`def Child : Parent` 单继承，链式查找统一终止于隐式根 `Object`（`def Foo` 等价
`def Foo : Object`）。方法沿链找最近实现；`super.` 前缀显式调**父类**实现：

```aria
def Animal {
    init(name) { this.name = name; }
    speak() { return this.name + " makes a sound"; }
}
def Dog : Animal {
    init(name) {
        super.init(name);
        this.tricks = 0;
    }
    speak() { return this.name + " barks (" + str(this.tricks) + " tricks)"; }
    learn() { this.tricks += 1; }
}
var d = Dog("rex");
println(d.speak());
d.learn();
d.learn();
println(d.speak());

def Puppy : Dog { }
var p = Puppy("bit");
println(p.speak());
```

输出：

```text
rex barks (0 tricks)
rex barks (2 tricks)
bit barks (0 tricks)
```

- `super.init(...)` 串构造链：`Dog` 的 `init` 先让 `Animal` 的 `init` 跑一遍再补自己的
  字段。子类不写 `init` 时直接继承父类的。
- `Puppy` 没定义 `speak`，沿链命中 `Dog` 的实现 -- 继承深度不设限。
- `super.` 只在**直接实例方法体**里可用，静态方法 / 顶层 / 嵌套 lambda 里写 `super` 是
  编译错误：

<!-- expect-error: SuperOutsideMethod -->
```aria
def S { fun sm() { return super.x; } }
```

```text
super.aria:1:27: Semantic: SuperOutsideMethod 'super' outside method
```

静态成员的继承是**读穿透、写遮蔽**：读 `Sub.tag` 沿链找，写 `Sub.tag = v` 落在子类自身：

```aria
def Base { var tag = "base"; }
def Sub : Base { }
println(Sub.tag);
Sub.tag = "sub";
println(Sub.tag);
println(Base.tag);
```

输出：

```text
base
sub
base
```

## 自定义可迭代

第 9 章说过 for-in 就是迭代协议的语法糖。自己的类实现 `iter()` / `has_next()` / `next()`
三个方法就能被 for-in 遍历。最省事的形态：`iter()` 返回 `this`，让对象自己当迭代器：

```aria
def Countdown {
    init(from) { this.cur = from; }
    iter() { return this; }
    has_next() { return this.cur > 0; }
    next() {
        this.cur -= 1;
        return this.cur + 1;
    }
}
var acc = 0;
for (v in Countdown(4)) { acc += v; }
println(acc);
```

输出：

```text
10
```

（4 + 3 + 2 + 1。）注意这个形态**每个对象只能有一个进行中的遍历**（游标就在自己身上）；
要支持并行遍历，让 `iter()` 返回一个专职迭代器实例（比如包一层游标类），每次调用
`iter()` 都造新的。

## 小结

- `def` 定义类；`init` 构造、`this` 指实例；实例字段动态、无需声明。
- 三类成员：实例方法（无 `fun`，有 `this`）、静态方法（有 `fun`，无 `this`）、静态
  变量（`var`）。读取链：实例字段 → 类链静态。
- `obj.method` 取绑定方法，一等值。
- `def Child : Parent` 单继承止于 `Object`；`super.` 调父实现，仅直接实例方法可用；
  静态成员读穿透、写遮蔽。
- 实现 `iter` / `has_next` / `next` 即可被 for-in 遍历。

## 练习

1. 写 `Stack` 类：`push(x)`、`pop()`、`peek()`（看栈顶不出栈）、`size()`。`pop` / `peek`
   在空栈时应抛出错误（用 `throw "empty stack";`，第 14 章预告）。
2. 写 `Vec2` 类：`init(x, y)`、`length_sq()`（模长平方，别开根号 -- aria 没有内建
   `sqrt`）、`scaled(k)` 返回新 `Vec2`。
3. 给第 6 章练习的 `dedup` 换个类实现：`Uniquer` 持一个 `seen` 映射，方法 `add(x)`
   返回「是否首次出现」。
4. 写 `Range2` 类：构造收 `from` / `to` / `step`（默认 1），实现三方法迭代协议，让
   `for (v in Range2(1, 10, 3))` 输出 1、4、7、10。

---

[上一章：match](11-match.md) · [下一章：运算符重载](13-operator-overloading.md)
