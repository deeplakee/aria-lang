# 第 6 章 列表

列表（`list`）是 aria 的动态数组：可变长、元素可为任意值的异构序列。打印形态是方括号
逗号分隔，字符串元素带引号：`[1, "two", nil]`。本章讲下标与负数下标、增删查的方法面、
排序，以及 aria 特色之一的区间切片。

## 字面量与基本性质

```aria
var xs = [1, "two", nil, true];
println(xs);
println(type(xs));
println(xs.size());
var grid = [[1, 2], [3, 4]];   # 嵌套
println(grid[1][0]);
```

输出：

```text
[1, "two", nil, true]
List
4
3
```

空列表 `[]`。`==` 按内容递归比较（`[1, [2]] == [1, [2]]` 为真），`===` 比身份（是不是
同一个列表对象）。

## 下标读写

整数下标从 0 起；**负数从尾计数**，`-1` 是末元素、`-size` 是首元素。读写同口径：

```aria
var xs = [10, 20, 30];
println(xs[0]);
println(xs[-1]);
println(xs[-3]);
xs[-1] = 33;
xs[0] += 5;
println(xs);
```

输出：

```text
10
30
10
[15, 20, 33]
```

越界与类型错误都是运行期错误，且**写下标不会自动增长列表**（追加请用 `push`）：

<!-- expect-error: IndexOutOfBounds -->
```aria
var xs = [1, 2];
println(xs[5]);
```

```text
Runtime: IndexOutOfBounds list index 5 out of range
  at <main> (idx.aria:2)
```

<!-- expect-error: IndexOutOfBounds -->
```aria
var ys = [1];
ys[1] = 2;
```

```text
Runtime: IndexOutOfBounds list index 1 out of range
  at <main> (grow.aria:2)
```

## 增删

| 方法 | 行为 |
| :--- | :--- |
| `push(x)` | 追加到末尾 |
| `pop()` | 移除并返回末元素；空表报错 |
| `insert(i, x)` | 在位置 `i` 前插入；负数从尾计数，`i == size` 即追加 |
| `remove(x)` | 移除**全部** `==` 命中的元素，返回是否命中（Bool） |
| `remove_at(i)` | 按位置移除并返回该元素；负数从尾计数 |
| `clear()` | 原地清空 |

```aria
var xs = [1, 2];
println(xs.push(3));
println(xs);
println(xs.pop());
xs.insert(0, 7);
xs.insert(-1, 8);
println(xs);
println(xs.remove(99));
println(xs.remove(8));
println(xs);
println(xs.remove_at(0));
println(xs);
```

输出：

```text
nil
[1, 2, 3]
3
[7, 1, 8, 2]
false
true
[7, 1, 2]
7
[1, 2]
```

逐步对照：`insert(-1, 8)` 在末元素**前**插入得 `[7, 1, 8, 2]`；`remove(8)` 移除的是值 8。
注意约定：**变更方法就地修改、返回 `nil`**（`remove` 例外返回 Bool），
**取值方法返回新值**（`pop` / `remove_at` 返回元素、切片返回新表）。
`remove` 与 `contains` / `find` 一样走 `==` 内容相等。

空表 `pop` 报错而不是返回哨兵值：

<!-- expect-error: IndexOutOfBounds -->
```aria
var last = [].pop();
```

```text
Runtime: IndexOutOfBounds pop from empty list
  at <main> (pop.aria:1)
```

## 查询

| 方法 | 行为 |
| :--- | :--- |
| `find(x)` | 首个 `==` 命中的下标；**未命中返 `nil`**（不是 -1） |
| `contains(x)` | 是否含 `==` 命中元素 |
| `size()` | 元素数 |
| `is_empty()` | 空表谓词 |

```aria
var xs = [10, 20, 30, 20];
println(xs.find(20));
println(xs.find(99));
println(xs.contains(30));
println(xs.is_empty());
println([].is_empty());
```

输出：

```text
1
nil
true
false
true
```

`find` 返 `nil` 与负下标组合时要小心：`xs[xs.find(v)]` 在未命中时不会报错，而是静默取到
末元素（`nil` 参与下标归一）。判断存在用 `contains`，取下标前先判 `nil`。

## 拼接与重复

`+` 把两个列表拼成一个新表（两侧都必须是列表）；`*` 按整数次数重复出新表。两者都产出
**新列表**，原表不变：

```aria
var xs = [1, 2];
println(xs + [3, 4]);
println(xs * 2);
println(xs);
println(xs * 0);
```

输出：

```text
[1, 2, 3, 4]
[1, 2, 1, 2]
[1, 2]
[]
```

元素是浅拷贝：嵌套的列表在两表间共享同一对象（要独立副本得逐元素另建）：

```aria
var inner = [7];
var two = [inner] * 2;
inner[0] = 8;
println(two);
```

输出：

```text
[[8], [8]]
```

复合赋值 `+=`/`*=` 同语义：`xs += [3]` 等价 `xs = xs + [3]`（左值只求值一次）。注意这是
**重绑新值**，旧表的其他别名看不到追加（要与别名共享变更用 `push`）。乘数必须是整数：
浮点一律报错，负数报错而不静默得空表：

<!-- expect-error: TypeMismatch -->
```aria
var xs = [1] * -1;
```

```text
Runtime: TypeMismatch __mul__ requires a non-negative integer, got -1
  at <main> (repeat.aria:1)
```

## 排序与连接

`sort()` **就地**升序排序（稳定序），要求全数值或全字符串；`reverse()` 就地整段反转；
`join(sep)` 把元素渲染成字符串后用 `sep` 连接，产新串：

```aria
var ys = [3, 1, 2];
println(ys.sort());
println(ys);
var words = ["banana", "apple", "cherry"];
words.sort();
println(words);
ys.reverse();
println(ys);
println(["a", "b", "c"].join("-"));
println([1, "x", nil].join(","));
```

输出：

```text
nil
[1, 2, 3]
["apple", "banana", "cherry"]
[3, 2, 1]
a-b-c
1,x,nil
```

混域排序直接报错（数值与字符串没有全序）：

<!-- expect-error: TypeMismatch -->
```aria
var xs = [1, "a"];
xs.sort();
```

```text
Runtime: TypeMismatch sort requires all numbers or all strings, got Int and String
  at <main> (sort.aria:2)
```

`sort` 不带比较函数参数。要按自定义规则排序，惯用法是自己写「按 key 归并/插入」的高阶
函数（结合第 5 章的闭包）。

## 切片：range 作下标

下标里放 range 就是切片，产出一个**新列表**，原表不变。区间语义与第 9 章的 range 一致：
`a..b` 含上界，`a...b` 不含上界，`a..` 无上界直到结尾。端点负数从尾计数；**倒序 range
产出倒序段**：

```aria
var xs = [10, 20, 30, 40, 50];
println(xs[1..3]);
println(xs[1...3]);
println(xs[3..]);
println(xs[-3..]);
println(xs[3..1]);
println(xs[2...2]);
println(xs[5..]);
println(xs);
```

输出：

```text
[20, 30, 40]
[20, 30]
[40, 50]
[30, 40, 50]
[40, 30, 20]
[]
[]
[10, 20, 30, 40, 50]
```

两个容易漏的口径：

- `xs[3..1]` 不是报错也不是空表，而是**倒着取** -- 倒序 range 表达的就是倒序段。
- `xs[5..]` 允许起点恰好等于长度，得空段；但 `xs[1..9]` 的上界越界要报错：

<!-- expect-error: IndexOutOfBounds -->
```aria
var xs = [10, 20, 30];
println(xs[1..9]);
```

```text
Runtime: IndexOutOfBounds slice range 1..9 out of range
  at <main> (slice.aria:2)
```

**切片是只读的** -- 对 range 下标赋值是类型错误（没有切片替换语法）：

<!-- expect-error: TypeMismatch -->
```aria
var xs = [10, 20, 30];
xs[0..1] = [9, 9];
```

```text
Runtime: TypeMismatch list index must be an integer, got Range
  at <main> (store.aria:2)
```

没有步长切片（`xs[1..9..2]` 不存在），隔位取用循环自己写。

## 小结

- 下标读写同口径：负数从尾计数，越界 / 非整数运行期报错，写不自动增长。
- 变更方法就地改、返 `nil`（`remove` 返 Bool）；`find` 未命中返 `nil` 而非 -1。
- `sort` 就地升序、限全数值或全字符串、无比较参数；`join` 在列表侧。
- range 下标即切片：闭 / 半开 / 无上界 / 负端点 / 倒序段；切片只读、产新表。

## 练习

1. 写 `dedup(xs)`：去掉重复元素并**保持首次出现顺序**（如 `[1, 2, 1, 3, 2]` 得
   `[1, 2, 3]`）。
2. 写 `merge_sorted(a, b)`：合并两个已升序的列表为一个新的升序列表（不许先拼接再
   `sort`）。
3. 用 `push` / `pop` 实现栈语义的 `fun stack_demo()`：入栈 1、2、3，再弹两次打印弹出的值。
4. 不运行先推理：`var xs = [1, 2, 3, 4, 5]; println(xs[-2...]);` 输出什么？验证你的答案
   （负端点从尾计数后按半开区间取）。

---

[上一章：闭包](05-closures.md) · [下一章：映射](07-maps.md)
