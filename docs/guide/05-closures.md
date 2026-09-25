# 第 5 章 闭包

lambda 能引用外层函数的局部变量 -- 被引用的变量被闭包「捕获」。aria 的捕获语义一句话：
**捕获即引用**。闭包拿到的是变量本身，不是变量当时的值拷贝；同一变量的多个闭包共享同一份
状态。这决定了 aria 里计数器、状态机、回调的写法，本章把这些模式过透。

## 捕获即引用

经典计数器：

```aria
fun make_counter() {
    var n = 0;
    return fun() {
        n += 1;
        return n;
    };
}
var next = make_counter();
println(next());
println(next());
println(next());
```

输出：

```text
1
2
3
```

`make_counter` 已经返回，局部变量 `n` 却活着，且每次调用 `next()` 都在改它。如果捕获是
值拷贝，三次调用都会打印 1。反过来看，闭包内的修改对「外面」同样可见：

```aria
var count = 0;
var bump = fun() { count += 1; };
bump();
bump();
println(count);
```

输出：

```text
2
```

## 每次 make 是独立实例

再次调用 `make_counter()` 会得到一套全新的 `n`，两个计数器互不干扰：

```aria
fun make_counter() {
    var n = 0;
    return fun() {
        n += 1;
        return n;
    };
}
var c = make_counter();
var d = make_counter();
println(c());
println(c());
println(d());
println(c());
```

输出：

```text
1
2
1
3
```

`d` 第一次调用得 1（自己的 `n`），`c` 的计数不受影响，第三次仍是 3。

## 多个闭包共享同一变量

同一轮 `make` 产出的多个闭包，捕获的是**同一个** `n`。用一个映射装一对存取函数，就成了
带状态的「对象」雏形（第 12 章的类是它的正式形态）：

```aria
fun make_account(start) {
    var amount = start;
    return {
        "deposit": fun(v) { amount += v; return amount; },
        "balance": fun() { return amount; }
    };
}
var acc = make_account(100);
println(acc["deposit"](50));
println(acc["balance"]());
```

输出：

```text
150
150
```

`deposit` 改的 `amount` 就是 `balance` 读的那个。

## 工厂：返回函数的函数

`make_adder` 是最小的参数化工厂 -- 捕获量由工厂参数决定：

```aria
fun make_adder(n) {
    return fun(x) { return x + n; };
}
var add5 = make_adder(5);
var add10 = make_adder(10);
println(add5(1));
println(add10(1));
println(add5(add10(1)));
```

输出：

```text
6
11
16
```

## for-in 的每轮都是新绑定

循环体内创建的闭包，捕获的是**那一轮的**循环变量 -- aria 的 for-in 每轮循环都开一个新
作用域，下一轮的 `i` 是一个新变量：

```aria
var fns = [];
for (i in 1..3) {
    fns.push(fun() { return i; });
}
println(fns[0]());
println(fns[1]());
println(fns[2]());
```

输出：

```text
1
2
3
```

三个闭包各自捕获了 1、2、3。从 JavaScript（`var` 时代）过来的读者会预期全是 3 -- aria
没有那个共享坑。反过来说，如果你想「闭包看到循环的最新值」，就不要依赖捕获，把值当参数
传进工厂函数。

## 高阶函数实战

把第 4 章的一等函数与本章的捕获合起来，就能写标准的 `map` / `filter`（aria 内建方法面
没有这两个，自己写两个即可）：

```aria
fun my_map(xs, f) {
    var out = [];
    for (x in xs) { out.push(f(x)); }
    return out;
}
fun my_filter(xs, pred) {
    var out = [];
    for (x in xs) {
        if (pred(x)) { out.push(x); }
    }
    return out;
}
var xs = [1, 2, 3, 4, 5, 6];
println(my_map(xs, fun(x) { return x * x; }));
println(my_filter(xs, fun(x) { return x % 2 == 0; }));
```

输出：

```text
[1, 4, 9, 16, 25, 36]
[2, 4, 6]
```

带累计状态的例子 -- 遍历一次求均值：

```aria
fun mean(xs) {
    var total = 0;
    var add = fun(x) { total += x; };
    for (x in xs) { add(x); }
    return total / xs.size();
}
println(mean([2, 4, 6, 8]));
```

输出：

```text
5
```

## 小结

- 捕获即引用：闭包持有变量本身，读改都作用于同一份状态。
- 每次执行外层函数产生一套新变量；同一轮里的多个闭包共享它们。
- for-in 每轮新作用域，循环内闭包捕获的是当轮的值。
- 「工厂返回闭包」是参数化行为的惯用法；容器 + 一组闭包可当轻量状态对象。

## 练习

1. 写 `make_counter(start, step)`：计数从 `start` 起、每调一次加 `step`。
   验证 `make_counter(10, 5)` 连调三次得 15、20、25。
2. 写 `my_reduce(xs, f, init)`：左折叠。用它对 `[1, 2, 3, 4]` 求和与求积。
3. 写 `memoized` 版斐波那契：外层存一个缓存 map，内层函数先查缓存再递归（提示：第 7 章
   的 `has` / 下标写；现在可以先用「list 里存两个值」的笨办法绕开 map）。
4. 预测下面程序的输出再运行验证：
   `var fs = []; var k = 0; while (k < 3) { fs.push(fun() { return k; }); k += 1; } println(fs[0]());`
   （注意与 for-in 的差别：这里 `k` 只声明了一次。）

---

[上一章：函数](04-functions.md) · [下一章：列表](06-lists.md)
