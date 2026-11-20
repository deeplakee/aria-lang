# 第 15 章 模块

用 `import` 可以导入文件模块和标准库，并在当前作用域绑定一个别名。一个 `.aria` 文件就是一个
模块，文件顶层的所有绑定（var / fun / def / import）自动成为模块成员。

## 导入名和别名

<!-- skip -->
```aria
import "util" as U;
```

三个要点：

- 导入名写在字符串里。导入文件模块时通常省略 `.aria` 扩展名。
- `as 别名` **强制必写** -- 模块永远以别名出现在你的作用域里，没有「导入即散装倒出」。
- `import` 是普通声明：放在模块顶层时 `U` 是模块全局，放在函数体 / 块内是局部绑定。

## 导入标准库

`std:` 开头的完整名字用于查询标准库。比如 `"std:math"` 提供数学函数和常量。

<!-- run: main.aria -->
```aria
import "std:math" as math;
import "std:math" as m;

println(math.sqrt(81));
println(math === m);
```

```text
9.0
true
```

标准库名区分大小写，`"std:Math"` 和 `"std:math.aria"` 都会报 `ModuleNotFound`。
项目中的 `math.aria` 可以用 `import "math" as local_math;` 单独导入。标准库成员同样只读。
数学函数和常量见[标准库参考](17-builtin-reference.md#stdmath-数学模块)。

## 双文件起步

目录布局（本章所有例子都在同一目录下跑 `build/aria main.aria`）：

```text
demo/
  main.aria
  util.aria
```

<!-- file: util.aria -->
```aria
# util.aria
var version = "1.0";

fun double(x) {
    return x * 2;
}

println("[util] module loaded");
```

<!-- run: main.aria -->
```aria
# main.aria
import "util" as U;

println(U.version);
println(U.double(21));

import "util" as U2;
println(U === U2);
```

输出：

```text
[util] module loaded
1.0
42
true
```

三个行为都在里面：

- **导入即执行**：`import` 语句执行时加载目标文件，模块顶层的语句（包括 `println`）当场
  跑一遍。`[util] module loaded` 打印在 main 的输出之前。
- **只执行一次**：`import "util"` 写两遍不会加载两次 -- 模块按命中文件的绝对路径登记，
  第二次直接返回已加载的模块对象，所以 `U === U2` 为真。
- `U.version` / `U.double(...)`：顶层绑定即成员，经别名点出来用。
- **模块体的「返回值」恒为模块对象**：裸 `return;` 可提前退出模块体（其后的语句不执行），
  带值的 `return 42;` 是编译错误--`import` 绑定到别名的始终是模块对象本身。

## 顶层绑定即成员（无 export）

不挑类型：顶层 `def` 的类、`fun`、`var`、以及模块自己 `import` 进来的其他模块，全部都是
成员，对导入方一视同仁：

<!-- file: shapes.aria -->
```aria
# shapes.aria
def Circle {
    init(r) { this.r = r; }
    area() { return 3 * this.r * this.r; }
}

var pi_digits = 3;

fun describe(c) {
    return "circle area ~= " + str(c.area());
}
```

<!-- run: main.aria -->
```aria
# main.aria
import "shapes" as S;

var c = S.Circle(2);
println(S.describe(c));
println(S.pi_digits);
```

输出：

```text
circle area ~= 12
3
```

模块里没有任何可见性修饰（没有 private / public）：顶层是什么，成员就是什么。反过来，
**函数体内**的局部变量不是成员 -- 想暴露就放顶层。

模块点号访问只查这些顶层绑定，不会回退到 Module 或 Object 的方法。未导出的
`U.init`、`U.__str__` 也按缺成员报错。模块可以自由导出 `is_a`、`name`、`__str__` 等名字，
它们都只是模块内容；`is_a(U, Module)` 检查模块对象身份，`str(U)` 查询模块显示协议，
两者都不执行同名导出。`U.f(args)` 与先取出 `U.f` 再调用含义一致，不向普通模块函数隐式传入 U。

## 路径解析：源根与相对导入

文件模块有两条解析规则：

- **非相对**：`import "util" as U;` -- 从**源根**搜起，先查入口脚本所在目录，再查配置根，
  加载首个命中的 `util.aria`。
- **相对**：`import "./sub/inner" as I;` -- 基点恒为**当前文件所在目录**，不逃到别的
  源根。`./` 前缀明确表达「相对本文件」。

相对导入配合嵌套导入，链式取值自然成立 -- 模块 import 的模块也是它的成员：

```text
demo/
  main.aria
  util.aria
  sub/
    inner.aria
```

<!-- file: sub/inner.aria -->
```aria
# sub/inner.aria
var tag = "inner";
```

<!-- file: util.aria -->
```aria
# util.aria
import "./sub/inner" as Inner;

var label = "utils";
```

<!-- run: main.aria -->
```aria
# main.aria
import "util" as U;

println(U.Inner.tag);
println(U.label);
```

输出：

```text
inner
utils
```

`U.Inner.tag` 一路点下去：`Inner` 是 util 的成员（它自己 import 的），`tag` 又是
inner 的成员。

找不到模块时，执行到 `import` 会抛出可捕获的 `ModuleNotFound`。

<!-- expect-error: ModuleNotFound -->
<!-- run: main.aria -->
```aria
import "nope" as N;
```

```text
Resource: ModuleNotFound module not found: 'nope'
  at <main> (main.aria:1)
```

## 成员只读

模块对导入方是只读的：读成员没问题，**跨模块写**一律报错 -- 想改状态就调模块提供的函数
（状态改动发生在模块自己文件里）：

<!-- file: util.aria -->
```aria
# util.aria
var version = "1.0";

fun bump() {
    version = "2.0";
    return version;
}
```

<!-- expect-error: TypeMismatch -->
<!-- run: main.aria -->
```aria
# main.aria
import "util" as U;

println(U.bump());

U.version = "3.0";
```

输出（最后一行后报错）：

```text
2.0
Runtime: TypeMismatch type Module does not support field assignment
  at <main> (main.aria:5)
```

读不存在的成员则报成员缺失：

<!-- file: util.aria -->
```aria
# util.aria
var version = "1.0";
```

<!-- expect-error: UndefinedProperty -->
<!-- run: main.aria -->
```aria
# main.aria
import "util" as U;

var v = U.absent;
```

```text
Runtime: UndefinedProperty <module util> has no member 'absent'
  at <main> (main.aria:3)
```

## 循环导入：拿到半初始化的模块

`a` 导入 `b`、`b` 又导入 `a` 时不会死循环：后到的请求直接拿到**半初始化**的 `a` -- 它
此刻只执行到 `import "./b"` 这一句，后面的绑定还没创建。读未完成的绑定报
`UndefinedProperty`，可 catch：

```text
demo/
  main.aria
  a.aria
  b.aria
```

<!-- file: a.aria -->
```aria
# a.aria
import "./b" as B;

var ready = true;

fun report() { return B.a_state; }
```

<!-- file: b.aria -->
```aria
# b.aria
import "./a" as A;

var a_state = "unknown";
try {
    a_state = str(A.ready);
} catch (e) {
    a_state = "half: " + str(e);
}
```

<!-- run: main.aria -->
```aria
# main.aria
import "a" as A;

println(A.ready);
println(A.report());
```

输出：

```text
true
half: Runtime: UndefinedProperty <module a> has no member 'ready'
```

b 里读 `A.ready` 时 a 还没执行到 `var ready`，于是走 catch。等一切加载完成，`A.ready`
正常是 `true`。循环依赖该当设计异味处理 -- 通常是「把两边共用的东西抽到第三个模块」的
信号；但语言不崩溃、错误可捕获，调试是可行的。

## 小结

- `import "导入名" as 别名;` 导入文件模块或以 `std:` 开头的标准库模块，别名必写。
- 顶层绑定（var / fun / def / import）即成员；裸名不自动导入。
- 文件模块按绝对规范路径只执行一次；非相对路径走源根，`./` 相对当前文件。
- 原生标准库模块按完整名创建一次，重复导入复用同一个模块对象。
- 成员对导入方只读；读写不存在分别报 `TypeMismatch` / `UndefinedProperty`。
- 循环导入拿到半初始化模块，读未完成绑定报可捕获的 `UndefinedProperty`。

## 练习

1. 把第 7 章的词频统计拆成两个文件：`textutil.aria` 提供 `tokenize(s)`（按空白切 +
  `lower()`），`main.aria` 导入它做计数。跑通后再 `import` 两次验证只加载一次（在
  `textutil.aria` 顶层放一句 `println` 观察）。
2. 建三层模块 `a` 导入 `b`、`b` 导入 `c`，`c` 里有 `var depth = 3;`，在 main 里经
  `A.B.C.depth` 取到 3。
3. 故意制造循环导入：`x` 与 `y` 互相导入，`y` 在顶层 catch 里记录「x 此时有几个成员」
   -- 想个办法观察（提示：y 里 catch 之后，把错误消息存进顶层 var，main 里读出来）。
4. 不运行先推理：main 里 `import "util" as U;` 之后 `U.double` 与直接把 `double` 的
  函数体抄进 main 各自调用，输出相同吗？模块边界改变了什么（提示：第 5 章的捕获 -- 
  模块顶层 var 对模块内函数是什么）？

---

[上一章：异常](14-exceptions.md) · [下一章：协程](16-coroutines.md)
