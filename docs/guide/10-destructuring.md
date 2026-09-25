# 第 10 章 解构

解构是把「一串值」按位置拆到多个名字上的一条语句。aria 的解构模式在三处通用：`var`
声明、for-in 目标、对已有变量的解构赋值。一套模式三种位置，是语言里少数「完全复用」的
设计 -- 学会一处就会全部。

## 模式长什么样

模式（pattern）只有四种构件：

| 构件 | 含义 |
| :--- | :--- |
| `name` | 绑定 / 赋值到这个名字 |
| `_` | 占位：跳过这个位置，什么都不绑 |
| `[p1, p2, ...]` | 嵌套列表模式，构件可任意嵌套 |
| `...name` | rest：把**剩余元素**收集成一个新列表绑到 name，只能在末尾 |

```aria
var xs = [1, 2, 3, 4];
var [a, b] = xs;          # 取前两个，多余忽略
println(a);
println(b);
var [h, _, t] = [1, 2, 3];  # _ 占位：中间的 2 被跳过
println(h);
println(t);
var [p, [q, r]] = [1, [2, 3]];  # 嵌套
println(p + q + r);
```

输出：

```text
1
2
1
3
6
```

三个口径：

- **多余忽略、不足报错**。位置 `i` 对应源 `list[i]`，源比模式短就是越界：

<!-- expect-error: IndexOutOfBounds -->
```aria
var [a, b] = [1];
```

```text
Runtime: IndexOutOfBounds list index 1 out of range
  at <main> (destructure.aria:1)
```

- `_` 不只是「可读性占位」：它**不产生取值动作**，源在该位置缺东西也不报错（`var [_, _] = [1];`
  合法，取 `1` 的位置之外什么都没发生）。
- 解构源目前是 **list 与 string**（string 按第 8 章的字节下标域逐位取，`var [x, y] = "hi";`
  得 `"h"` 与 `"i"`）。map 不能这样解构 -- 它没有「第 0 个位置」的概念。

## rest：收集剩余

`...name` 收集从当前位置到结尾的所有元素，产出一个**新列表**：

```aria
var [h, ...t] = [1, 2, 3, 4];
println(h);
println(t);
var [only, ...empty] = [9];
println(empty);
var [...all] = [1, 2];
println(all);
var [_, ...rest] = [1, 2, 3];   # _ 也占一个位置
println(rest);
```

输出：

```text
1
[2, 3, 4]
[]
[1, 2]
[2, 3]
```

head/tail 是它的标准用法。注意 `_` 位也计入位置（`rest` 从第 2 个起）。rest 必须绑名，
`..._` 是编译期错误（既然不想要就该不写）：

<!-- expect-error: InvalidPattern -->
```aria
var [..._] = [1];
```

```text
rest.aria:1:9: Syntax: InvalidPattern rest pattern cannot bind '_'
```

string 源配 rest 按字节切片取后缀（语料实证过的口径）：

```aria
var [head, ...tail] = "abc";
println(head);
println(tail);
```

输出：

```text
a
bc
```

## for-in 目标

for-in 的循环变量位置放的就是同一个模式，每轮对 `next()` 的产出解构。map 每轮产出
`[k, v]`，正好拆开：

```aria
var m = {"ada": 36, "bob": 41};
for ([name, age] in m) {
    println(name + ": " + str(age));
}
```

输出：

```text
ada: 36
bob: 41
```

嵌套列表同样直接拆：

```aria
var pairs = [[1, "one"], [2, "two"]];
for ([n, word] in pairs) {
    println(word);
}
```

输出：

```text
one
two
```

（map 的两键顺序这里恰好按插入序，但正式口径仍是未规定 -- 见第 7 章。）

## 解构赋值：对已有变量

不带 `var` 的 `[a, b] = ...` 写**已经存在的**变量（未声明的名字报错，不会隐式创建）。
最经典的用法是交换，不需要临时变量：

```aria
var a = 1;
var b = 2;
[a, b] = [b, a];
println(a);
println(b);
```

输出：

```text
2
1
```

交换之所以成立，是因为**右侧整体只求值一次**：先算出 `[b, a]` 这个新列表，再按位置写回。
这条性质在右侧有函数调用时看得更清楚：

```aria
fun make() {
    println("making");
    return [7, 8];
}
var x = 0;
var y = 0;
[x, y] = make();
println(x + y);
```

输出：

```text
making
15
```

解构赋值本身也是表达式，值就是右侧的值：

```aria
var x = 0;
var y = 0;
var z = ([x, y] = [7, 8]);
println(z);
```

输出：

```text
[7, 8]
```

## 小结

- 模式 = 名字 / `_` / 嵌套列表 / `...rest`，在 var、for-in、赋值三处通用。
- 多余忽略、不足报 `IndexOutOfBounds`；`_` 跳过且不取值。
- rest 收集剩余为新列表，只能居末、必须绑名；string 源按字节域取。
- 解构赋值写已有变量，右侧只求值一次，`[a, b] = [b, a]` 交换成立。

## 练习

1. 写 `minmax(xs)`：返回 `[最小值, 最大值]` 二元列表，调用处用解构接收并打印两个值。
2. 用解构交换实现斐波那契迭代：`var [a, b] = [0, 1];` 循环 10 次 `[a, b] = [b, a + b];`，
   打印第 10 轮后的 `b`。
3. 写 `fun split_first(xs)`：返回 `[首元素, 余下列表]`，调用处一行解构接收，分别打印。
   空列表怎么处理？（返回 nil 并在调用处判断，或抛异常 -- 第 14 章之后回来重看这题。）
4. 不运行先推理：`var [a, _, ...rest] = [1, 2, 3, 4, 5];` 之后 `a` 与 `rest` 各是什么？
   验证你的答案。

---

[上一章：区间与迭代](09-ranges-and-iteration.md) · [下一章：match](11-match.md)
